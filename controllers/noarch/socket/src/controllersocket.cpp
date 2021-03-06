#include "controllersocket.h"

#include <boost/filesystem.hpp>

namespace bfs = boost::filesystem;

using boost::asio::ip::udp;

namespace visionsystem
{

ControllerSocket::ControllerSocket( VisionSystem * vs, std::string sandbox )
: Controller(vs, "controllersocket", sandbox),
  io_service_(), io_service_th_(0), cams_(0)
{
}

ControllerSocket::~ControllerSocket()
{
    for(size_t i = 0; i < cams_.size(); ++i)
    {
        delete cams_[i];
    }
}

bool ControllerSocket::pre_fct( std::vector<GenericCamera *> & cams )
{
    bfs::path sandbox( get_sandbox() );
    whiteboard_write<CameraSocket*>("streamcamera", 0);
    if( bfs::is_directory(sandbox) )
    {
        std::vector<bfs::path> ls_path;
        copy(bfs::directory_iterator(sandbox), bfs::directory_iterator(), back_inserter(ls_path));
        for(std::vector< bfs::path >::const_iterator it = ls_path.begin(); it != ls_path.end(); ++it)
        {
            if( bfs::is_regular_file(*it) && bfs::extension(*it) == ".conf" )
            {
                CameraSocket * cam = new CameraSocket(io_service_);
                cam->read_config_file( (*it).string().c_str() );
                if(cam->is_active())
                {
                    cam->start_cam();
                }
                if(cam->from_stream())
                {
                    whiteboard_write<CameraSocket*>("streamcamera", cam);
                }
                cams.push_back(cam);
                cams_.push_back(cam);
            }
        }
    }
    return true;
}

void ControllerSocket::preloop_fct()
{
    io_service_th_ = new boost::thread(boost::bind(&boost::asio::io_service::run, &io_service_));
}

void ControllerSocket::loop_fct()
{
    for(size_t i = 0; i < cams_.size(); ++i)
    {
        if(cams_[i]->is_active() && cams_[i]->has_data())
        {
            Frame * vsframe = cams_[i]->_buffer.pull();
            std::memcpy( vsframe->_data, cams_[i]->get_data(), cams_[i]->get_data_size() );
            vsframe->_data_size = cams_[i]->get_data_size();
            cams_[i]->_buffer.push(vsframe);
        }
    }
}

bool ControllerSocket::post_fct()
{
    for(size_t i = 0; i < cams_.size(); ++i)
    {
        cams_[i]->stop_cam();
    }
    io_service_.stop();
    io_service_th_->join();
    delete io_service_th_;
    io_service_th_ = 0;
    return true;
}

void ControllerSocket::get_cameras(std::vector<GenericCamera*> & cams)
{
    for(size_t i = 0; i < cams_.size(); ++i)
    {
        cams.push_back(cams_[i]);
    }
}

} // namespace visionsystem

