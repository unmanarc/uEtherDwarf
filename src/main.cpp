#include <thread>

#include <mdz_prg_service/application.h>
#include <mdz_net_sockets/acceptor_multithreaded.h>
#include <mdz_mem_vars/a_uint16.h>
#include <mdz_mem_vars/a_uint32.h>
#include <mdz_mem_vars/a_bool.h>
#include <mdz_mem_vars/a_ipv4.h>

#include <inttypes.h>
#include <sys/time.h>
#include <fstream>

#include "pkt_dissector.h"
#include "app_options.h"
#include "tls_callbacks.h"
#include "virtiface_reader.h"
#include "tls_ping.h"

#include "config.h"

#include <inttypes.h>

using namespace Mantids;
using namespace Mantids::Memory;
using namespace Mantids::Application;
using namespace Mantids::Network::Sockets;
using namespace Mantids::Network::Interfaces;

class uEtherDwarfApp : public Mantids::Application::Application
{
public:
    uEtherDwarfApp() {
    }

    void _shutdown()
    {
        log->log0(__func__,Logs::LEVEL_INFO, "SIGTERM received.");
    }

    void _initvars(int argc, char *argv[], Arguments::GlobalArguments * globalArguments)
    {
        globalArguments->setInifiniteWaitAtEnd(true);

        /////////////////////////
        globalArguments->setVersion( atoi(PROJECT_VER_MAJOR), atoi(PROJECT_VER_MINOR), atoi(PROJECT_VER_PATCH), "" );

        globalArguments->setLicense("GPLv3");
        globalArguments->setAuthor("Aarón Mizrachi");
        globalArguments->setEmail("aaron@unmanarc.com");
        globalArguments->setDescription(PROJECT_DESCRIPTION);

        globalArguments->addCommandLineOption("TAP Interface", 'i', "interface" , "Interface Name"  , "utap0",                      Abstract::Var::TYPE_STRING);
        globalArguments->addCommandLineOption("TAP Interface",   0, "persistent", "Persistent Mode"  , "true",                      Abstract::Var::TYPE_BOOL);

        globalArguments->addCommandLineOption("TLS Options", '9', "notls" , "Disable the use of TLS"  , "false",                    Abstract::Var::TYPE_BOOL);
        globalArguments->addCommandLineOption("TLS Options", '4', "ipv4" , "Use only IPv4"  , "true",                               Abstract::Var::TYPE_BOOL);
        globalArguments->addCommandLineOption("TLS Options", 'y', "cafile" , "X.509 Certificate Authority Path", "",                Abstract::Var::TYPE_STRING);
        globalArguments->addCommandLineOption("TLS Options", 'k', "keyfile" , "X.509 Private Key Path (For listening mode)"  , "",  Abstract::Var::TYPE_STRING);
        globalArguments->addCommandLineOption("TLS Options", 'c', "certfile" , "X.509 Certificate Path (For listening mode)"  , "", Abstract::Var::TYPE_STRING);
        globalArguments->addCommandLineOption("TLS Options", 'l', "listen" , "Use in listen mode"  , "false",                       Abstract::Var::TYPE_BOOL);


        globalArguments->addCommandLineOption("TLS Options", 'p', "port" , "Port"  , "443",                                         Abstract::Var::TYPE_UINT16);
        globalArguments->addCommandLineOption("TLS Options", 'a', "addr" , "Address"  , "*",                                        Abstract::Var::TYPE_STRING);
        globalArguments->addCommandLineOption("TLS Options", 't', "threads" , "Max Concurrent Connections (Threads)"  , "1024",     Abstract::Var::TYPE_UINT16);
        globalArguments->addCommandLineOption("TLS Options", 'e', "pingevery" , "Ping every (Seconds), 0 to disable"  , "10",       Abstract::Var::TYPE_UINT32);


        globalArguments->addCommandLineOption("Scripts", 'u', "up" , "Up Script (executed when connection is up)"  , "",            Abstract::Var::TYPE_STRING);
        globalArguments->addCommandLineOption("Scripts", 'w', "down" , "Down Script (executed when connection is down)"  , "",      Abstract::Var::TYPE_STRING);

        globalArguments->addCommandLineOption("Authentication", 'f', "peersfile" , "Formatted multi line file (IP:PSK:MAC, first line is for myself)"  , "", Abstract::Var::TYPE_STRING);
        globalArguments->addCommandLineOption("Other Options", 's', "sys" , "Journalctl Log Mode (don't print colors or dates)"  , "false",                  Abstract::Var::TYPE_BOOL);

#ifndef _WIN32
        globalArguments->addCommandLineOption("Other Options", 'x', "uid" , "Drop Privileged to UID"  , "0",                        Abstract::Var::TYPE_UINT16);
        globalArguments->addCommandLineOption("Other Options", 'g', "gid" , "Drop Privileged to GID"  , "0",                        Abstract::Var::TYPE_UINT16);
#endif

    }

    bool loadTLSParameters(Socket_TCP *sock, bool clientMode, bool printUsing = true)
    {
        if (!appOptions.cafile.empty() || clientMode)
        {
            if (access(appOptions.cafile.c_str(),R_OK))
            {
                log->log0(__func__,Logs::LEVEL_CRITICAL, "X.509 Certificate Authority File Not Found.");
                return false;
            }
            ((Socket_TLS *)sock)->keys.loadCAFromPEMFile(appOptions.cafile.c_str());

            // Only print this in the header...
            if (!printUsing)
                log->log0(__func__,Logs::LEVEL_INFO, "The peer will be authenticated with the TLS Certificate Authority");
        }
        else
            log->log0(__func__,Logs::LEVEL_WARN, "The peer can come without TLS signature, Internal VPN IP will be exposed.");

        // Need server certificates:
        if (!clientMode || !appOptions.keyfile.empty())
        {
            if (access(appOptions.keyfile.c_str(),R_OK))
            {
                log->log0(__func__,Logs::LEVEL_CRITICAL, "X.509 Private Key not found.");
                return false;
            }
            if (printUsing) log->log0(__func__,Logs::LEVEL_INFO, "Using peer TLS private key: %s",appOptions.keyfile.c_str());
            ((Socket_TLS *)sock)->keys.loadPrivateKeyFromPEMFile(appOptions.keyfile.c_str());
        }

        if (!clientMode || !appOptions.certfile.empty())
        {
            if (access(appOptions.certfile.c_str(),R_OK))
            {
                log->log0(__func__,Logs::LEVEL_CRITICAL, "X.509 Certificate not found.");
                return false;
            }
            if (printUsing) log->log0(__func__,Logs::LEVEL_INFO, "Using peer TLS certificate: %s",appOptions.certfile.c_str());
            ((Socket_TLS *)sock)->keys.loadPublicKeyFromPEMFile(appOptions.certfile.c_str());
        }
        return true;
    }

    bool _config(int argc, char *argv[], Arguments::GlobalArguments * globalArguments)
    {
        Socket_TLS::prepareTLS();
        bool configUseFancy    = !((Abstract::BOOL *)globalArguments->getCommandLineOptionValue("sys"))->getValue();

        log = new Logs::AppLog();
        log->setPrintEmptyFields(true);
        log->setUserAlignSize(1);
        log->setUsingAttributeName(false);
        log->setUsingColors(configUseFancy);
        log->setUsingPrintDate(configUseFancy);
        log->setModuleAlignSize(36);
        appOptions.log = log;

        fprintf(stderr,"# Arguments: %s\n", globalArguments->getCurrentProgramOptionsValuesAsBashLine().c_str());

        std::string passFile = globalArguments->getCommandLineOptionValue("peersfile")->toString();

        if ( !passFile.empty() )
        {
            std::ifstream file(passFile);
            if (file.is_open()) {

                std::string myUserConfigLine, othersConfigLine;
                if (!std::getline(file, myUserConfigLine))
                {
                    log->log0(__func__,Logs::LEVEL_CRITICAL, "Peers File '%s' requires at least 2 lines (one for me, and the other for other peers)", passFile.c_str());
                    exit(-15);
                }

                if (!appOptions.localTapOptions.setPeerDefinition(myUserConfigLine,log,0))
                    exit(-14);

                for (uint32_t lineNo = 1 ; std::getline(file, othersConfigLine);lineNo++)
                {
                    sPeerDefinition loptions;
                    if (!loptions.setPeerDefinition(othersConfigLine,log,lineNo))
                        exit(-16);
                    if (loptions.cidrNetmask!=32)
                    {
                        log->log0(__func__,Logs::LEVEL_CRITICAL, "Netmask is not /32 in configuration line number: %d", lineNo);
                        exit(-17);
                    }
                    if (appOptions.peersDefinition.find(loptions.aIpAddr.s_addr)!=appOptions.peersDefinition.end())
                    {
                        log->log0(__func__,Logs::LEVEL_CRITICAL, "IP address repeated in configuration line number: %d", lineNo);
                        exit(-13);
                    }

                    appOptions.peersDefinition[loptions.aIpAddr.s_addr] = loptions;
                }
                file.close();
            }
            else
            {
                log->log0(__func__,Logs::LEVEL_CRITICAL, "Failed to open peers file '%s'", passFile.c_str());
                exit(-13);
            }
        }

#ifdef _WIN32
        ULONG adapterIndex = 10000;
#endif
        std::string tapReadInterfaceName;
        ethhdr tapIfaceEthAddress;

        appOptions.ifaceName = globalArguments->getCommandLineOptionValue("interface")->toString();

        NetIfConfig tapIfaceCfg;
        tapIfaceCfg.setUP(true);
        if ( ! appOptions.tapIface.start(&tapIfaceCfg,appOptions.ifaceName) )
        {
            log->log0(__func__,Logs::LEVEL_CRITICAL, "Failed to open TAP Interface %s - %s",appOptions.tapIface.getInterfaceRealName().c_str(),appOptions.tapIface.getLastError().c_str());
            exit(-5);
        }

        sleep(1);

        tapIfaceEthAddress = tapIfaceCfg.getEthernetAddress();

        if (!memcmp(tapIfaceEthAddress.h_dest,"\0\0\0\0\0\0",6))
        {
            log->log0(__func__,Logs::LEVEL_CRITICAL, "Unable to get Hardware Address from TAP interface %s",
                      appOptions.tapIface.getInterfaceRealName().c_str(),
                      appOptions.tapIface.getLastError().c_str());
            exit(-4);
        }
        tapReadInterfaceName = appOptions.tapIface.getInterfaceRealName();
#ifdef _WIN32
        adapterIndex = appOptions.tapIface.getWinTapAdapterIndex();
        log->log0(__func__,Logs::LEVEL_INFO,  "Using TAP-Windows6 Version: %s", appOptions.tapIface.getWinTapVersion().toString().c_str());
        log->log0(__func__,Logs::LEVEL_INFO,  "TAP Network Interface Info: %s", appOptions.tapIface.getWinTapDeviceInfo().c_str());
        log->log0(__func__,Logs::LEVEL_DEBUG, "TAP Network Interface Log Line: %s", appOptions.tapIface.getWinTapLogLine().c_str());
#endif

#ifdef _WIN32
        log->log0(__func__,Logs::LEVEL_INFO, "TAP Network Interface IDX=%lu NAME=%s HWADDR=%s Ready.", adapterIndex,
          #else
        log->log0(__func__,Logs::LEVEL_INFO, "TAP Network Interface NAME=%s HWADDR=%s Ready.",
          #endif
                  tapReadInterfaceName.c_str(),
                  Abstract::MACADDR::_toString(tapIfaceEthAddress.h_dest).c_str());

        if (((Abstract::BOOL *)globalArguments->getCommandLineOptionValue("persistent"))->getValue())
        {
            if (appOptions.tapIface.setPersistentMode(true))
                log->log0(__func__,Logs::LEVEL_INFO, "TAP Network Interface is now in persistent mode.");
            else
                log->log0(__func__,Logs::LEVEL_ERR, "Failed to set up TAP Network Interface in persistent mode.");
        }

        appOptions.tapHwAddrHash    = Abstract::MACADDR::_toHash(tapIfaceEthAddress.h_dest);
        appOptions.ipv4             = ((Abstract::BOOL *)globalArguments->getCommandLineOptionValue("ipv4"))->getValue();
        appOptions.notls            = ((Abstract::BOOL *)globalArguments->getCommandLineOptionValue("notls"))->getValue();

        if (appOptions.notls)
            log->log0(__func__,Logs::LEVEL_WARN, "Proceding in plain-text mode, eavesdropping communications will be easy!!!");

        appOptions.upScript     = globalArguments->getCommandLineOptionValue("up")->toString();
        appOptions.downScript   = globalArguments->getCommandLineOptionValue("down")->toString();
        appOptions.cafile       = globalArguments->getCommandLineOptionValue("cafile")->toString();
        appOptions.certfile     = globalArguments->getCommandLineOptionValue("certfile")->toString();
        appOptions.keyfile      = globalArguments->getCommandLineOptionValue("keyfile")->toString();
        appOptions.addr         = globalArguments->getCommandLineOptionValue("addr")->toString();
        appOptions.port         = ((Abstract::UINT16 *)globalArguments->getCommandLineOptionValue("port"))->getValue();
        appOptions.listenMode   = ((Abstract::BOOL *)globalArguments->getCommandLineOptionValue("listen"))->getValue();
        appOptions.threadsLimit = ((Abstract::UINT16 *)globalArguments->getCommandLineOptionValue("threads"))->getValue();
        appOptions.pingEvery    = ((Abstract::UINT32 *)globalArguments->getCommandLineOptionValue("pingevery"))->getValue();

        sock = (appOptions.notls?new Socket_TCP:new Socket_TLS ) ;
        sock->setUseIPv6( !appOptions.ipv4 );

        appOptions.uid = ((Abstract::UINT16 *)globalArguments->getCommandLineOptionValue("uid"))->getValue();
        appOptions.gid = ((Abstract::UINT16 *)globalArguments->getCommandLineOptionValue("gid"))->getValue();

        tapIfaceCfg.setIPv4Address(appOptions.localTapOptions.aIpAddr,
                                   Abstract::IPV4::_fromCIDRMask(appOptions.localTapOptions.cidrNetmask)
                                   );

        if (!tapIfaceCfg.apply())
        {
            log->log0(__func__,Logs::LEVEL_CRITICAL, "Failed to configure TAP Interface Parameters...");
            exit(-601);
        }

        // Change the UID/GID values to be applied before _start.
        globalArguments->setUid(appOptions.uid);
        globalArguments->setGid(appOptions.gid);

        // Check if listen mode.
        if (appOptions.listenMode)
        {
            if (!appOptions.notls)
            {
                if (!loadTLSParameters(sock,false,false))
                    exit(-105);
            }

            if (!sock->listenOn( appOptions.port, appOptions.addr.c_str() ))
            {
                log->log0(__func__,Logs::LEVEL_CRITICAL, "Unable to listen at %s:%" PRIu16,appOptions.addr.c_str(), appOptions.port);
                exit(-20);
                return false;
            }

            multiThreadedAcceptor.setAcceptorSocket(sock);
            multiThreadedAcceptor.setCallbackOnConnect(TLS_Callbacks::onConnect,&appOptions);
            multiThreadedAcceptor.setCallbackOnInitFail(TLS_Callbacks::onInitFailed,this);
            multiThreadedAcceptor.setCallbackOnTimedOut(TLS_Callbacks::onTimeOut,this);
            multiThreadedAcceptor.setMaxConcurrentClients(appOptions.threadsLimit);
        }
        else
        {
            if (!appOptions.notls)
            {
                if (!loadTLSParameters(sock,true,false))
                    exit(-105);
            }
        }


        return true;
    }


    int _start(int argc, char *argv[], Arguments::GlobalArguments * globalArguments)
    {
        std::thread( virtIfaceReader, &appOptions ).detach();
        std::thread( tlsPeersPingThread, &appOptions ).detach();

        if (appOptions.listenMode)
        {
            multiThreadedAcceptor.startThreaded();
            log->log0(__func__,Logs::LEVEL_INFO, "VPN Server Loaded @%s:%" PRIu16, appOptions.addr.c_str(),appOptions.port);
        }
        else
        {
            for (;;)
            {
                if (sock)
                    delete sock;

                sock = (appOptions.notls?new Socket_TCP:new Socket_TLS ) ;
                sock->setUseIPv6( !appOptions.ipv4 );

                if (!appOptions.notls)
                {
                    if (!loadTLSParameters(sock,true))
                        exit(-104);
                }

                log->log0(__func__,Logs::LEVEL_INFO, "Connecting to %s://%s:%" PRIu16 "...", appOptions.notls?"tcp":"tls",appOptions.addr.c_str(),appOptions.port);

                if (sock->connectTo(appOptions.addr.c_str(),appOptions.port))
                    TLS_Callbacks::onConnect(&appOptions,sock, appOptions.addr.c_str(),true);
                else
                {
                    log->log0(__func__,Logs::LEVEL_ERR, "%s.",  sock->getLastError().c_str());

                    if (!appOptions.notls)
                    {
                        for (const auto & i: ((Socket_TLS *)sock)->getTLSErrorsAndClear())
                            log->log0(__func__,Logs::LEVEL_ERR, "TLS Error - [%s]", i.c_str());
                    }
                }

                sleep(5);
            }
        }

        return 0;
    }

private:
    sAppOptions appOptions;
    Logs::AppLog * log;
    Socket_TCP *sock;
    Acceptors::MultiThreaded multiThreadedAcceptor;
};

int main(int argc, char *argv[])
{
    uEtherDwarfApp * main = new uEtherDwarfApp;
    return StartApplication(argc,argv,main);
}

