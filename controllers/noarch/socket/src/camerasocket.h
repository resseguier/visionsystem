#ifndef _H_CAMERASOCKET_H_
#define _H_CAMERASOCKET_H_

#include <boost/bind.hpp>
#include <boost/asio.hpp>

#include <configparser/configparser.h>
#include <visionsystem/genericcamera.h>
#include <vision/image/image.h>
#include <vision/win32/windows.h>

#include <vision/io/H264Decoder.h>

#ifdef WIN32
#define VS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define VS_PLUGIN_EXPORT
#endif

using boost::asio::ip::udp;

namespace visionsystem
{

class CameraSocket : public visionsystem::GenericCamera, public configparser::WithConfigFile
{
public:
    CameraSocket(boost::asio::io_service & io_service);

    ~CameraSocket();

    /* Camera socket specific */
    bool is_ready() { return cam_ready_; }

    void start_cam();

    bool has_data();

    /* Stream camera specific functions */

    void next_cam() { next_cam_ = true; }

    VS_PLUGIN_EXPORT void request_cam(const std::string & cam_name);

    /* End of stream camera specific functions */

    unsigned char * get_data();

    unsigned int get_data_size();

    unsigned int get_frame() { return frame_; }

    void stop_cam();

    /* Camera methods to implement */
    vision::ImageRef get_size() { return img_size_; }

    bool from_stream() { return from_stream_; }

    bool is_active() { return active_; }

    bool is_raw() { return raw_; }

    visionsystem::FrameCoding get_coding() { return img_coding_; }

    float get_fps() { return 1e6/fps_; }

    std::string get_name() { return name_; }
private:
    /* Configparser method */
    void parse_config_line ( std::vector<std::string> & line );

    /* Async sockets callbacks */
    void handle_receive_from(const boost::system::error_code & error,
                                size_t bytes_recvd);
    void handle_send_to(const boost::system::error_code & error,
                                size_t bytes_send);

    void handle_timeout(const boost::system::error_code & error);

public:
    /* Camera related parameters */
    vision::ImageRef img_size_;
    bool active_;
    visionsystem::FrameCoding img_coding_;
    unsigned int fps_; /* read as FPS but stored as time intervall between each frame */
    unsigned int frame_;
    timeval previous_frame_t_;
    std::string name_;
    /* This parameter indicates if the camera is fed from stream2socket plugin */
    bool from_stream_;
    bool next_cam_;
    bool request_cam_;
    std::string request_name_;
private:
    /* CameraSocket specific */
    bool cam_ready_;
    bool has_data_;
    std::string server_name_;
    short server_port_;
    bool reverse_connection_;
    short port_;

    /* Raw transfer */
    bool raw_;

    /* H.264 support */
    bool data_compress_;
    vision::H264Decoder * m_decoder;

    /* Socket members */
    boost::asio::io_service & io_service_;
    udp::socket socket_;
    udp::endpoint receiver_endpoint_;
    udp::endpoint sender_endpoint_;
    std::string request_;
    /* client request */
    enum { chunk_size_ = 32769 };
    unsigned char chunk_buffer_[chunk_size_];
    uint8_t chunkID_;
    boost::asio::deadline_timer timeout_timer_;

    /* Camera related */
    vision::Image<unsigned char, vision::MONO> * shw_img_mono_;
    vision::Image<unsigned char, vision::MONO> * rcv_img_mono_;
    vision::Image<uint32_t, vision::RGB> * shw_img_rgb_;
    vision::Image<uint32_t, vision::RGB> * rcv_img_rgb_;
    unsigned char * shw_img_raw_data_;
    unsigned char * rcv_img_raw_data_;

    unsigned int buffersize_;

    bool verbose_;
};

}

#endif
