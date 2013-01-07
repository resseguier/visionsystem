#include "stream2socket.h"

inline void remove_alpha(unsigned char * data_in, unsigned int nb_pixels, unsigned char * data_out)
{
    for(unsigned int i = 0; i < nb_pixels; ++i)
    {
        memcpy(&(data_out[3*i]), &(data_in[4*i]), 3);
    }
}

using boost::asio::ip::udp;

namespace visionsystem
{

Stream2Socket::Stream2Socket( visionsystem::VisionSystem * vs, std::string sandbox )
: Plugin( vs, "stream2socket", sandbox ),
  io_service_(), io_service_th_(0), 
  socket_(0),
  server_name_(), server_port_(4242),
  port_(0), chunkID_(0),
  raw_(false),
  ready_(false),
  reverse_connection_(false),
  active_cam_(0), cams_(0), 
  compress_data_(false), encoder_(0),
  send_img_(0), img_lock_(false),
  next_cam_(false), request_cam_(false), request_name_(""),
  verbose_(false)
{}

Stream2Socket::~Stream2Socket()
{
    delete send_img_;
    delete[] client_data_;
    delete[] send_buffer_;
    delete socket_;
    delete encoder_;
}

bool Stream2Socket::pre_fct()
{
    std::string filename = get_sandbox() + std::string("/stream2socket.conf");

    try
    {
        read_config_file(filename.c_str());
    }
    catch(std::string msg)
    {
        throw(std::string("stream2socket will not work without a correct stream2socket.conf config file"));
    }

    /* Store all active cameras */
    std::vector<Camera *> cameras = get_all_cameras();
    for(size_t i = 0; i < cameras.size(); ++i)
    {
        if(cameras[i]->is_active())
        {
            cams_.push_back(cameras[i]);
        }
    }
    if(cams_.size() == 0)
    {
        throw(std::string("No active cameras in the server, stream2socket cannot operate"));
    }
    if(compress_data_)
    {
        encoder_ = new vision::H264Encoder(cams_[0]->get_size().x, cams_[0]->get_size().y, cams_[0]->get_fps());
        #if Vision_HAS_LIBAVCODEC != 1
        std::cerr << "[stream2socket] H.264 support not built in vision library, Compress option is not usable" << std::endl;
        #endif
    }

    vision::ImageRef image_size = cams_[0]->get_size();
    register_to_cam< vision::Image<uint32_t, vision::RGB> >( cams_[0], 10 ) ;
    send_img_ = new vision::Image<uint32_t, vision::RGB>(image_size);
    send_img_data_size_ = 3 * send_img_->pixels;
    send_img_raw_data_ = (unsigned char *)(send_img_->raw_data);

    socket_ = new udp::socket(io_service_);
    socket_->open(udp::v4());
    client_data_ = new char[max_request_];
    send_buffer_ = new unsigned char[send_size_];
    
    if(reverse_connection_)
    {
        /* DNS Resolution */
        {
            udp::resolver resolver(io_service_);
            std::stringstream ss;
            ss << server_port_;
            udp::resolver::query query(udp::v4(), server_name_, ss.str());
            receiver_endpoint_ = *resolver.resolve(query);
        }
        if(verbose_)
        {
            std::cout << "[stream2socket] connected to " << server_name_ << ":" << server_port_ << " to stream images" << std::endl;
        }
        ready_ = true;
        /* Get ready for eventual request */
        socket_->async_receive_from(
           boost::asio::buffer(client_data_, max_request_), sender_endpoint_,
           boost::bind(&Stream2Socket::handle_receive_from, this, 
               boost::asio::placeholders::error,
               boost::asio::placeholders::bytes_transferred));
    }
    else
    {
        socket_->bind(udp::endpoint(udp::v4(), port_));
        if(verbose_)
        {
            std::cout << "[stream2socket] Waiting for request now" << std::endl;
        }
        socket_->async_receive_from(
           boost::asio::buffer(client_data_, max_request_), sender_endpoint_,
           boost::bind(&Stream2Socket::handle_receive_from, this, 
               boost::asio::placeholders::error,
               boost::asio::placeholders::bytes_transferred));
    }

    return true ;
}

void Stream2Socket::preloop_fct()
{
    io_service_th_ = new boost::thread(boost::bind(&boost::asio::io_service::run, &io_service_));
}

void Stream2Socket::loop_fct()
{
    vision::Image<uint32_t, vision::RGB> * img = dequeue_image< vision::Image<uint32_t, vision::RGB> > (cams_[active_cam_]);

    if(!img_lock_ && ready_)
    {
        if(compress_data_)
        {
            vision::H264EncoderResult res = encoder_->Encode(*img);
            send_img_data_size_ = res.frame_size;
            send_img_raw_data_  = res.frame_data;
        }
        else if(raw_)
        {
            memcpy(send_img_raw_data_, img->raw_data, img->data_size);
            send_img_data_size_ = img->data_size;
        }
        else
        {
            remove_alpha((unsigned char*)(img->raw_data), img->pixels, send_img_raw_data_);
        }
        chunkID_ = 0;
        img_lock_ = true;
        send_buffer_[0] = chunkID_;
        size_t send_size = 0;
        if( (chunkID_ + 1)*(send_size_ - 1) > send_img_data_size_ )
        {
            send_size = send_img_data_size_ - chunkID_*(send_size_ - 1) + 1;
        }
        else
        {
            send_size = send_size_;
        }
        std::memcpy(&(send_buffer_[1]), &(send_img_raw_data_[chunkID_*(send_size_ - 1)]), send_size - 1);
        if(verbose_)
        {
            std::cout << "[stream2socket] Sending data to client, data size: " << send_size << std::endl;
        }
        socket_->async_send_to(
            boost::asio::buffer(send_buffer_, send_size), receiver_endpoint_,
            boost::bind(&Stream2Socket::handle_send_to, this, 
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
    }

    enqueue_image< vision::Image<uint32_t, vision::RGB> >(cams_[active_cam_], img);

    if(next_cam_)
    {
        unregister_to_cam< vision::Image<uint32_t, vision::RGB> > (cams_[active_cam_]);
        active_cam_ = (active_cam_ + 1) % cams_.size();
        register_to_cam< vision::Image<uint32_t, vision::RGB> > (cams_[active_cam_], 10);
        next_cam_ = false;
    }
    if(request_cam_)
    {
        for(size_t i = 0; i < cams_.size(); ++i)
        {
            if(cams_[i]->get_name() == request_name_)
            {
                unregister_to_cam< vision::Image<uint32_t, vision::RGB> > (cams_[active_cam_]);
                active_cam_ = i;
                register_to_cam< vision::Image<uint32_t, vision::RGB> > (cams_[active_cam_], 10);
            }
        }
        request_cam_ = false;
    }
}

bool Stream2Socket::post_fct()
{
    unregister_to_cam< vision::Image<uint32_t, vision::RGB> >(cams_[active_cam_]);

    socket_->close();
    io_service_.stop();
    io_service_th_->join();
    delete io_service_th_;
    io_service_th_ = 0;

    return true;
}

void Stream2Socket::handle_receive_from(const boost::system::error_code & error,
                                 size_t bytes_recvd)
{
    if(!error && bytes_recvd > 0)
    {
        std::string client_message(client_data_);
        if(verbose_)
        {
            std::cout << "[stream2socket] Got request " << client_message << std::endl;
        }
        if(client_message == "get")
        {
            /* Client request: reset sending vision::Image */
            receiver_endpoint_ = sender_endpoint_;
            ready_ = true;
        }
        else if(client_message == "next")
        {
            /* Client requested to change camera stream */
            next_cam_ = true;
        }
        else if(client_message.substr(0, 8) == "request ")
        {
            request_cam_ = true;
            request_name_ = client_message.substr(8);
        }
    }
    else
    {
        if(verbose_)
        {
            std::cout << "[stream2socket] Reception error, waiting for next message" << std::endl;
        }
        if(error) { std::cerr << error.message() << std::endl; }
    }
    socket_->async_receive_from(
        boost::asio::buffer(client_data_, max_request_), sender_endpoint_,
        boost::bind(&Stream2Socket::handle_receive_from, this, 
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
}

void Stream2Socket::handle_send_to(const boost::system::error_code & error,
                            size_t bytes_send)
{
    if(error) { std::cerr << error.message() << std::endl; }
    if(bytes_send < send_size_) // after last packet sent
    {
        img_lock_ = false;
        if(verbose_)
        {
            std::cout << "[stream2socket] Image sent, waiting for next image" << std::endl;
        }
        return;
    }
    chunkID_++;
    send_buffer_[0] = chunkID_;
    size_t send_size = 0;
    if( (chunkID_ + 1)*(send_size_ - 1) > send_img_data_size_ )
    {
        send_size = send_img_data_size_ - chunkID_*(send_size_ - 1) + 1;
    }
    else
    {
        send_size = send_size_;
    }
    std::memcpy(&(send_buffer_[1]), &(send_img_raw_data_[chunkID_*(send_size_ - 1)]), send_size - 1);
    if(verbose_)
    {
        std::cout << "[stream2socket] More data to send, next packet size: " << send_size << std::endl;
    }
    socket_->async_send_to(
        boost::asio::buffer(send_buffer_, send_size), sender_endpoint_,
        boost::bind(&Stream2Socket::handle_send_to, this,
        boost::asio::placeholders::error,
        boost::asio::placeholders::bytes_transferred));
}

void Stream2Socket::parse_config_line( std::vector<std::string> & line )
{
    if( fill_member(line, "Port", port_) )
        return;

    if( fill_member(line, "Compress", compress_data_) )
    {
#if Vision_HAS_LIBAVCODEC != 1
        if(compress_data_)
        {
            std::cerr << "[WARNING] You configured stream2socket to compress data without H.264 support" << std::endl;
        }
#endif
        return;
    }

    if( fill_member(line, "Raw", raw_) )
        return;

    if( fill_member(line, "ReverseConnection", reverse_connection_) )
        return;

    if( fill_member(line, "ServerName", server_name_) )
        return;

    if( fill_member(line, "ServerPort", server_port_) )
        return;

    if( fill_member(line, "Verbose", verbose_) )
        return;
}

} // namespace visionsystem

