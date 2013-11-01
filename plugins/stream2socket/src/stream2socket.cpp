#ifdef VS_HAS_CONTROLLER_SOCKET
    #include "camerasocket.h"
#endif

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
  raw_(false),
  reverse_connection_(false),
  cams_(0),
  compress_data_(false),
  ports_(0),
  cam_names_(0),
  processes_(0),
  verbose_(false)
{}

Stream2Socket::~Stream2Socket()
{
    for(size_t i = 0; i < processes_.size(); ++i)
    {
        delete processes_[i];
    }
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

    if( ports_.size() > 1 && (reverse_connection_ && server_names_.size() != ports_.size()) )
    {
        throw(std::string("stream2socket configured for reverse connection but mismatching ports and servers"));
    }
    if( cam_names_.size() != 0 && cam_names_.size() != ports_.size() )
    {
        throw(std::string("stream2socket configured for specific streaming but mismatching ports and cameras"));
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

    if(cam_names_.size() != 0 && cams_.size() < cam_names_.size() )
    {
        throw(std::string("stream2socket configured for specific streaming but not enough cameras to stream"));
    }

    for(size_t i = 0; i < ports_.size(); ++i)
    {
        Camera * cam = cams_[0];
        std::string server_name = "";
        if(cam_names_.size())
        {
            cam = get_camera(cam_names_[i]);
        }
        if(server_names_.size())
        {
            server_name = server_names_[i];
        }
        if(!cam)
        {
            std::stringstream ss; ss << "stream2socket cannot find camera named " << cam_names_[i] << std::endl;
            throw(ss.str());
        }
        Stream2SocketProcess * process = new Stream2SocketProcess(io_service_, ports_[i], 0, cam, compress_data_, raw_, reverse_connection_, server_name, verbose_);
        register_to_cam< vision::Image<uint32_t, vision::RGB> >( cam, 10 ) ;
        processes_.push_back(process);
    }

    return true ;
}

void Stream2Socket::preloop_fct()
{
    io_service_th_ = new boost::thread(boost::bind(&boost::asio::io_service::run, &io_service_));
}

void Stream2Socket::loop_fct()
{
    for(size_t i = 0; i < processes_.size(); ++i)
    {
        Stream2SocketProcess * process = processes_[i];
        vision::Image<uint32_t, vision::RGB> * img = dequeue_image< vision::Image<uint32_t, vision::RGB> > (process->cam_);

        if(!process->img_lock_ && process->ready_)
        {
            if(compress_data_)
            {
                vision::H264EncoderResult res = process->encoder_->Encode(*img);
                process->send_img_data_size_ = res.frame_size;
                process->send_img_raw_data_  = res.frame_data;
            }
            else if(raw_)
            {
                memcpy(process->send_img_raw_data_, img->raw_data, img->data_size);
                process->send_img_data_size_ = img->data_size;
            }
            else
            {
                remove_alpha((unsigned char*)(img->raw_data), img->pixels, process->send_img_raw_data_);
            }
            process->img_lock_ = true;
            process->send_buffer_[0] = 0;
            size_t send_size = 0;
            if( (0 + 1)*(process->send_size_ - 1) > process->send_img_data_size_ )
            {
                send_size = process->send_img_data_size_ - 0*(process->send_size_ - 1) + 1;
            }
            else
            {
                send_size = process->send_size_;
            }
            std::memcpy(&(process->send_buffer_[1]), &(process->send_img_raw_data_[0*(process->send_size_ - 1)]), send_size - 1);
            if(verbose_)
            {
                std::cout << "[stream2socket] Sending data to client, data size: " << send_size << std::endl;
            }
            for(size_t id = 0; id < process->receivers_endpoint_.size(); ++id)
            {
                process->chunkIDs_[id] = 0;
                process->socket_->async_send_to(
                    boost::asio::buffer(process->send_buffer_, send_size), process->receivers_endpoint_[id],
                    boost::bind(&Stream2SocketProcess::handle_send_to, process,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred, id));
            }
        }

        enqueue_image< vision::Image<uint32_t, vision::RGB> >(process->cam_, img);

        if(process->next_cam_)
        {
            unregister_to_cam< vision::Image<uint32_t, vision::RGB> > (process->cam_);
            process->active_cam_ = (process->active_cam_ + 1) % cams_.size();
            register_to_cam< vision::Image<uint32_t, vision::RGB> > (cams_[process->active_cam_], 10);
            process->cam_ = cams_[process->active_cam_];
            process->next_cam_ = false;
        }
        if(process->request_cam_)
        {
            if(raw_)
            {
    #ifdef VS_HAS_CONTROLLER_SOCKET
                CameraSocket * cam = dynamic_cast<CameraSocket*>(process->cam_);
                if(cam && cam->from_stream())
                {
                    cam->request_cam(process->request_name_);
                }
    #endif
            }
            else
            {
                for(size_t i = 0; i < cams_.size(); ++i)
                {
                    if(cams_[i]->get_name() == process->request_name_)
                    {
                        unregister_to_cam< vision::Image<uint32_t, vision::RGB> > (process->cam_);
                        process->active_cam_ = i;
                        register_to_cam< vision::Image<uint32_t, vision::RGB> > (cams_[process->active_cam_], 10);
                        process->cam_ = cams_[process->active_cam_];
                    }
                }
            }
            process->request_cam_ = false;
        }
    }
}

bool Stream2Socket::post_fct()
{
    for(size_t i = 0; i < processes_.size(); ++i)
    {
        Stream2SocketProcess * process = processes_[i];

        unregister_to_cam< vision::Image<uint32_t, vision::RGB> >(process->cam_);

        process->socket_->close();
    }
    io_service_.stop();
    io_service_th_->join();
    delete io_service_th_;
    io_service_th_ = 0;

    return true;
}

void Stream2Socket::parse_config_line( std::vector<std::string> & line )
{
    if( fill_member(line, "Port", ports_) )
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

    if( fill_member(line, "ServerName", server_names_) )
        return;

    if( fill_member(line, "ServerPort", ports_) )
        return;

    if( fill_member(line, "Verbose", verbose_) )
        return;

    if( fill_member(line, "Cameras", cam_names_) )
        return;
}

} // namespace visionsystem

