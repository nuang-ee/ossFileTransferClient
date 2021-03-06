/*
* Copyright (c) 2020 Hunesion Inc.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; 
* either version 2.1 of the License, or (at your option) any later version.
* See the file COPYING included with this distribution for more information.
* https://github.com/HuneOpenUp/ossFileTransferClient/blob/master/COPYING
*/

#include "SocketServer.h"
#include "Logger.h"
#include "sys_utils.h"
#include "StringUtils.h"
#include "urlencode.h"
#include "FileUtils.h"
#include "UrlRedirection.h"
#include "Clipboard.h"
#include "model/GlobalVar.h"
#include "transfer/Download.h"
#include <memory>


#define FTC_SOCKET_DATA_FLAG_CLIPBOARD              "cb"
#define FTC_SOCKET_DATA_FLAG_URL_REDIRECTION        "ur"


BEGIN_FTC_CORE
SocketServer* SocketServer::s_instance = nullptr ;
volatile bool SocketServer::s_stop = false ;  

SocketServer *SocketServer::getInstance(){
    SocketServer* rv = SocketServer::s_instance ; 
    if (!rv){
        rv = new(std::nothrow)SocketServer(); 
        SocketServer::s_instance = rv ; 
    }
    return rv ; 
}



static gboolean socketServerConnectCallback(GThreadedSocketService *service,
               GSocketConnection      *connection,
               GObject                *source_object,
               gpointer                user_data) {
    SocketServer *socketServer = NULL;
    GInputStream *istream = NULL;
    GOutputStream *ostream = NULL;
    GError *error = NULL;
    std::string data;
    char buffer[1024] = { 0, };
    gsize readTotalSize = 0, readSize = 0, totalLength = 0;

    FTC_LOG_DEBUG("socket_server_connect_cb (Started) Service RefCount = %d IS MAIN THREAD = %d", G_OBJECT(service)->ref_count, ftc_is_main_thread()); 

    if (! user_data) {
        return false ;
    }
    socketServer = (SocketServer*)user_data;

    try
    {
        istream = g_io_stream_get_input_stream(G_IO_STREAM(connection));    
        if (! istream) {
            throw std::runtime_error("Input Stream 얻기 실패");
        }

        do
        {
            readSize = g_input_stream_read(istream, buffer, 1024, NULL, &error);

            if (readSize <= 0) {
                break;
            }
            data.append(buffer, readSize);
            readTotalSize += readSize;
        } while (true);

        socketServer->work(data);
    }
    catch(const std::runtime_error& e)
    {
        FTC_LOG("socketServerConnectCallback exception %s", e.what());
    }

    FTC_LOG_DEBUG("socket_server_connect_cb (Stoped) Service RefCount = %d IS MAIN THREAD = %d", G_OBJECT(service)->ref_count, ftc_is_main_thread()); 

    if (error) {
        g_error_free(error);
        error = NULL;
    }

    return false ; 
}


void SocketServer::destroyInstance(){
    
    if (SocketServer::s_instance){
        SocketServer::s_instance->stop(); 
        delete SocketServer::s_instance; 
        SocketServer::s_instance = nullptr ; 
    }

}

SocketServer::SocketServer()
{

}

SocketServer::~SocketServer(){

    for (auto it : this->_sockets) {
        if (it.listening_addr){
            FTC_LOG_DEBUG("[Port %d] Listener Address RefCount = %d", it.port, G_OBJECT(it.listening_addr)->ref_count); 
            g_object_unref(it.listening_addr); 
            it.listening_addr = NULL ; 
        }
        if (it.service){
            FTC_LOG_DEBUG("[Port %d] Service RefCount = %d", it.port, G_OBJECT(it.service)->ref_count); 
            g_object_unref(it.service); 
            it.service = NULL ; 
        }
    }
} 

bool SocketServer::start() {
    //  기본 수신 포트
    this->start(12000);
}

bool SocketServer::start(int port){
    bool rv = false ; 
    GError *error = NULL ; 
    GInetAddress *iaddr = NULL ; 
    GSocketAddress *saddr = NULL ; 
    Socket socket ;
    if (s_stop) return rv ; 

    for (auto it : this->_sockets) {
        if (it.port == port) {
            if (it.service) {
                if (g_socket_service_is_active(it.service)){
                    rv = true ; 
                }else {
                    g_socket_service_start(it.service); 
                    if (g_socket_service_is_active(it.service)){
                        rv = true ; 
                    }
                }
                return rv ;
            }
        }
    }

    socket.port = port;

    iaddr = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
    if (!iaddr) goto END ; 
    saddr = g_inet_socket_address_new (iaddr, socket.port);
    if (!saddr) goto END ; 
    g_object_unref (iaddr);
    iaddr = NULL ; 
    socket.service = g_threaded_socket_service_new(5); 
    
    if (! socket.service) goto END ;

    if (!g_socket_service_is_active(socket.service)){
        g_object_unref(socket.service); 
        socket.service = NULL ; 
        goto END ; 
    }
    
    g_socket_listener_add_address (G_SOCKET_LISTENER (socket.service),
                                 saddr,
                                 G_SOCKET_TYPE_STREAM,
                                 G_SOCKET_PROTOCOL_TCP,
                                 NULL,
                                 &(socket.listening_addr),
                                 &error);

    g_object_unref(saddr); 
    saddr = NULL ; 

    if (error){
        if (g_socket_service_is_active(socket.service)){
            g_socket_service_stop(socket.service); 
        }
        g_object_unref(socket.service); 
        socket.service = NULL ; 
        goto END ; 
    }

    g_signal_connect(socket.service, "run", G_CALLBACK(socketServerConnectCallback), this); 

    this->_sockets.push_back(socket);

    FTC_LOG("Socket Server Started : %d", socket.port);
    rv = true ; 

END : 
    if (iaddr){
        g_object_unref(iaddr); 
        iaddr = NULL ; 
    }

    if (saddr){
        g_object_unref(saddr); 
        saddr = NULL ; 
    }

    if (error){
        g_error_free(error); 
        error = NULL ; 
    }

    return rv ; 
}


bool SocketServer::stop(){
    bool rv = false ; 
    if (s_stop) return rv ; 

    for (auto it : this->_sockets) {
        if (it.service && g_socket_service_is_active(it.service) ){
            g_socket_service_stop(it.service); 
        }   
    }

    FTC_LOG("Socket Server Stoped");

    s_stop = true ; 
    rv = true ; 
    return rv ; 
}

bool SocketServer::work(std::string data) {
    bool rv = false;
    std::vector<std::string> splitData;
    std::string portStr, flag;
    int port;

    FTC_LOG("Socket Data : %s", data.c_str());

    splitData = StringUtils::splitToVector(data, '&');
    flag = splitData[2];

    if (flag == FTC_SOCKET_DATA_FLAG_URL_REDIRECTION) {
        rv = this->urlRedirection(data, splitData);
    } else if (flag == FTC_SOCKET_DATA_FLAG_CLIPBOARD) {
        rv = this->clipboard(data, splitData);
    } else {
        rv = this->autoDownload(data, splitData);
    }

    return rv;
}

bool SocketServer::clipboard(std::string &data, std::vector<std::string> &splitData)
{
    bool rv = false;
    Clipboard *clipboard = Clipboard::getInstance();
    std::list<std::string> fileList;
    std::string &requestInfoUid = splitData[4];
    std::string &localIp = splitData[1];

    fileList = downloadFile(requestInfoUid, localIp, "/tmp");
    if (fileList.size() <= 0) {
        return rv;
    }   

    for (auto &it : fileList) {
        clipboard->loadFile(it.c_str());
    }

    rv = true;

    return rv;
}

bool SocketServer::urlRedirection(std::string &data, std::vector<std::string> &splitData)
{
    bool rv = false;
    std::list<std::string> fileList;
    std::string &requestInfoUid = splitData[4];
    std::string &localIp = splitData[1];

    fileList = downloadFile(requestInfoUid, localIp, "/tmp");
    if (fileList.size() <= 0) {
        return rv;
    }

    //  파일을 읽고 인터넷 브라우저를 실행한다.
    //
    for (auto &it : fileList) {
        loadUrl(it.c_str());
    }

    rv = true;
    return rv;
}

bool SocketServer::autoDownload(std::string &data, std::vector<std::string> &splitData)
{
    bool rv = false;
    std::string &requestInfoUid = splitData[3];
    std::string &localIp = splitData[1];


    return rv;
}

std::list<std::string> SocketServer::downloadFile(const std::string &requestInfoUid, const std::string &localIp, const std::string &dir)
{
    Transfer::Download *download = NULL;
    std::list<std::string> rv;

    download = Transfer::Download::getInstance();
    if (! download) {
        return rv;
    }

    auto downRequest = download->getDownloadList(requestInfoUid, localIp);
    if (! downRequest) {
        return rv;
    }

    downRequest->setDirectory(dir);

    auto downFileList = downRequest->getDownlaodFiles();
    for (auto &it : downFileList) {
        if (download->downloadFile(it) == false) {
            FTC_LOG("%s download fail : %s", it->getFilename().c_str(), it->getErrorMsg().c_str());
        }
    }

    for (auto &it : downFileList) {
        std::string path;

        path = dir + "/" + it->getFilename();
        rv.push_back(path);
    }

    return rv;
}

END_FTC_CORE
