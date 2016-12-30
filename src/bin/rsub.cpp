#include "redux/application.hpp"
#include "redux/logging/logger.hpp"
#include "redux/debugjob.hpp"
#include "redux/momfbd/momfbdjob.hpp"
#include "redux/network/tcpconnection.hpp"
#include "redux/network/host.hpp"
#include "redux/util/arrayutil.hpp"
#include "redux/util/stringutil.hpp"

#include <sstream>
#include <thread>

#include <boost/bind.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/program_options.hpp>

using namespace redux::logging;
using namespace redux::util;
using namespace redux::network;
using namespace redux;

using namespace std;


namespace {

    const string logChannel = "rsub";

    // define options specific to this binary
    bpo::options_description getOptions( void ) {

        bpo::options_description options( "Program Options" );
        options.add_options()
        ( "master,m", bpo::value<string>()->default_value( "localhost" ),
          "Hostname/IP of a master to connect to."
          " The environment variable RDX_MASTER can be used to override the default value." )
        ( "port,p", bpo::value<string>()->default_value( "30000" ),
          "Port to use when connecting to a master."
          " The environment variable RDX_PORT can be used to override the default value." )
        ( "priority", bpo::value<int>()->default_value( 10 ), "Job priority" )
        ( "reg_alpha", bpo::value<float>(), "REG_ALPHA override" )
        ( "force,f", "Overwrite output file if exists" )
        ( "kill,k", "Send exit command to Server." )
        ( "swap,s", "swap mode: write auxiliary data to files instead of keeping it in memory (compatibility flag, always enabled)" )
        ( "config,c", bpo::value< vector<string> >()->multitoken(), "Configuration file(s) to process." )
        ( "name", bpo::value<string>(), "Name to use for the supplied configurations." )
        ( "simxy", bpo::value<string>(), "(x,y) coordinate[s] of subimages to restore" )
        ( "simx", bpo::value<string>(), "x coordinate[s] of subimages to restore" )
        ( "simy", bpo::value<string>(), "y coordinate[s] of subimages to restore" )
        ( "imgn,n", bpo::value<string>(), "Image numbers" )
//        ( "sequence", bpo::value<string>(), "sequence number to insert in filename template." )
        ( "print,P", "(debug) print the parsed configuration to console and exit without uploading." )
        ( "no-check", "Don't verify the configuration." )
        ( "output-dir,O", bpo::value<string>(), "Output directory. If left blank, the current directory is used.")
        ( "output-file,o", bpo::value<string>(), "Output file base names." )
        ( "init", bpo::value<string>()->implicit_value(""), "File with initial values for alpha. If no argument is provided, the output is used.")
        ;

        return options;
    }

    // define environment variables to use as defaults if the corresponding command-line option is not specified
    string environmentMap( const string &envName ) {

        static map<string, string> vmap;
        if( vmap.empty() ) {
            vmap["RDX_VERBOSITY"] = "verbosity";  // For debugging this might be convenient.
            vmap["RDX_MASTER"] = "master";        // If it exists, it will override the default value (localhost) above
            vmap["RDX_PORT"] = "port";            // If it exists, it will override the default value (30000) above
        }
        map<string, string>::const_iterator ci = vmap.find( envName );
        if( ci == vmap.end() ) {
            return "";
        }
        else {
            return ci->second;
        }
    }
}


void killServer(TcpConnection::Ptr conn, Logger& logger) {
    
    Host::HostInfo me, master;
    uint8_t cmd = CMD_CONNECT;
    boost::asio::write(conn->socket(),boost::asio::buffer(&cmd,1));
    boost::asio::read(conn->socket(),boost::asio::buffer(&cmd,1));

    if( cmd == CMD_AUTH ) {
        // implement
    }
    if( cmd == CMD_CFG ) {  // handshake requested
        LOG << "Requesting server to shutdown..." << ende;
        *conn << me;
        *conn >> master;
        boost::asio::read(conn->socket(),boost::asio::buffer(&cmd,1));       // ok or err
    }
    if( cmd != CMD_OK ) {
        LOG_ERR << "Handshake with server failed." << ende;
        return;
    }
   
    cmd = CMD_DIE;
    boost::asio::write(conn->socket(),boost::asio::buffer(&cmd,1));
   
}


void uploadJobs(TcpConnection::Ptr conn, vector<Job::JobPtr>& jobs, int prio, Logger& logger) {
    
    Host::HostInfo me, master;
    uint8_t cmd = CMD_CONNECT;
    try {
        boost::asio::write(conn->socket(),boost::asio::buffer(&cmd,1));
        boost::asio::read(conn->socket(),boost::asio::buffer(&cmd,1));

        if( cmd == CMD_AUTH ) {
            // implement
        }
        if( cmd == CMD_CFG ) {  // handshake requested
            *conn << me;
            *conn >> master;
            boost::asio::read(conn->socket(),boost::asio::buffer(&cmd,1));       // ok or err
        }
        if( cmd != CMD_OK ) {
            LOG_ERR << "Handshake with server failed." << ende;
            return;
        }
       
        // all ok, upload jobs.
        uint64_t jobsSize(0);
        for( auto &job: jobs ) {
            if( job ) {
                job->info.priority = prio;
                jobsSize += job->size();
            }
        }
        
        shared_ptr<char> buf( new char[jobsSize+sizeof(uint64_t)+1], []( char* p ){ delete[] p; } );
        char* ptr = buf.get()+sizeof(uint64_t)+1;
        uint64_t packedBytes(0);
        for( auto &job: jobs ) {
            if( job ) {
                packedBytes += job->pack(ptr+packedBytes);
            }
        }
        
        uint64_t totalSize = packedBytes+sizeof(uint64_t)+1;                    // cmd & blockSize will be sent before block
        ptr = buf.get();
        pack( ptr, CMD_ADD_JOB);
        pack( ptr+1, packedBytes );
        
        //conn->asyncWrite( buf, packedBytes );
        conn->syncWrite( buf.get(), totalSize );
        //boost::asio::write(conn->socket(),boost::asio::buffer(buf.get(),packedBytes));

        boost::asio::read(conn->socket(),boost::asio::buffer(&cmd,1));

        bool swap_endian = (me.littleEndian != master.littleEndian);
        uint64_t count, received;
        ptr = buf.get();

        if( cmd == CMD_OK ) {
            received = boost::asio::read( conn->socket(), boost::asio::buffer( ptr, sizeof(uint64_t) ) );
            if( received == sizeof(uint64_t) ) {
                unpack(ptr, count, swap_endian);
                size_t thisSize = count*sizeof(uint64_t);
                received = boost::asio::read( conn->socket(), boost::asio::buffer( ptr, thisSize ) );
                if( received == thisSize ) {
                    if( count ) LOG << "Upload of " << count << " job(s) completed successfully. " << printArray(reinterpret_cast<size_t*>(buf.get()),count,"IDs") << ende;
                } else {
                    LOG_ERR << "Failed to read job IDs.  received=" << received << " thisSize=" << thisSize << ende;
                }
            } else LOG_ERR << "Failed to read number of job IDs." << ende;
        } else {
            LOG_ERR << "Failure while sending jobs  (server reply = " << (int)cmd << "   " << bitString(cmd) << ")" << ende;
        }
        
        vector<string> messages;
        *conn >> messages;
        if( !messages.empty() ) {
            string msgText = "Server messages:";
            for( auto& msg: messages ) {
                msgText += "\n\t" + msg;
            }
            LOG_WARN << "Error uploading jobs: " << msgText << ende;
        }

    }
    catch( const exception &e ) {
        LOG_ERR << "Error uploading jobs: " << e.what() << ende;
    }


}


bool replace(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = str.find(from);
    bool found = false;
    while(start_pos != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        found = true;
        start_pos = str.find(from);
    }
    return found;
}


string filterOldCfg(string filename, string jobname, string logfile, string outputDir ) {
    
    std::ifstream in(filename, std::ios::in | std::ios::binary);
    if (!in) {
        throw (errno);
    }

    std::string text;
    in.seekg (0, std::ios::end);
    text.reserve (in.tellg());
    in.seekg (0, std::ios::beg);
    std::copy ( (std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>(), std::back_inserter (text));
    in.close();

    replace(text, " ", "");     // spaces not allowed in cfg-values (e.g. DIVERSITY=0.00 mm)
    bool found = replace(text, "channel{", "channel {");
    found |= replace(text, "object{", "object {");
    found |= replace(text, "=", " ");
    
    if (found) { // old cfg file, wrap inside momfbd { ... }
        text = "momfbd { \n" + text;
        text += "NAME " + jobname + "\n";
        text += "LOGFILE " + logfile + "\n";
        text += "OUTPUT_DIR " + outputDir + "\n";
        text += "\n}";
    }

    return text;
}


int main (int argc, char *argv[]) {
    
    bpo::variables_map vm;
    bpo::options_description programOptions = getOptions();

    try {
        bpo::options_description& allOptions = Application::parseCmdLine( argc, argv, vm, &programOptions );

        // load matched environment variables according to the environmentMap() above.
        bpo::store( bpo::parse_environment( allOptions, environmentMap ), vm );
#if BOOST_VERSION > 104800  // TODO check which version notify appears in
        vm.notify();
#endif
    }
    catch( const exception &e ) {
        cerr << "Error parsing commandline: " << e.what() << endl;// << programOptions << endl;
        return EXIT_FAILURE;
    }


    try {
        
        string globalLog;
        if( vm.count ("log-file") ) {
            vector<string> logFiles = vm["log-file"].as<vector<string>>();
            if( logFiles.size() ) {
                globalLog = logFiles[0];
            }
            if( logFiles.size() > 1 ) {
                cerr << "Only 1 log-file supported at the moment. Using: " << globalLog << endl;
            }
        }

        vm.erase("log-file");       // always log to cout for rsub
        vm.insert( std::make_pair("log-stdout", bpo::variable_value()) );
        
        Logger logger( vm );
        bpt::ptree momfbd;
        
        if( !vm.count ("config") ) {
            LOG_FATAL << "No configuration file supplied." << ende;
            return 0;
        }

        vector<string> files = vm["config"].as<vector<string>>();

        string globalName;
        if( vm.count ("name") ) {
            globalName = vm["name"].as<string>();
        }

        boost::filesystem::path outputDir = boost::filesystem::current_path();
        if( vm.count ("output-dir") ) {
            boost::filesystem::path tmpPath = vm["output-dir"].as<string>();
            if( isRelative( tmpPath ) && !outputDir.empty() ) {
                outputDir = outputDir / tmpPath;
            }
        }

        stringstream filteredCfg;
        for( auto it: files ) {
            string bn = boost::filesystem::basename(it);
            string jobName = globalName.empty() ? bn : globalName;
            string logFile = globalLog.empty() ? bn + ".log" : globalLog;
            string tmpS = filterOldCfg(it, jobName, logFile, outputDir.string()) + "\n";
            filteredCfg.write(tmpS.c_str(),tmpS.size());
        }

        bpt::read_info (filteredCfg , momfbd);
        bool check = (vm.count ("no-check") == 0 && !vm.count ("print"));
        vector<Job::JobPtr> jobs = Job::parseTree (vm, momfbd, check);
        
        if( jobs.empty() ) {
            LOG_WARN << "No jobs to upload." << ende;
            return EXIT_SUCCESS;
        }
        
        if( vm.count ("reg_alpha") ) {       // FIXME: this is just while testing...
            for( auto & job : jobs ) {
                static_pointer_cast<momfbd::MomfbdJob>(job)->reg_alpha = vm["reg_alpha"].as<float>();
            }
        }
        
        if (vm.count ("print")) {       // dump configuration to console and exit
            bpt::ptree dump;
            for( auto & job : jobs ) {
                job->getPropertyTree( &dump );
            }
            bpt::write_info( cout<<endl, dump );
            return EXIT_SUCCESS;
        }
        
        boost::asio::io_service ioservice;
        auto conn = TcpConnection::newPtr(ioservice);
        conn->connect( vm["master"].as<string>(), vm["port"].as<string>() );

        if( conn->socket().is_open() ) {
            if(vm.count ("kill")) {
                killServer(conn, logger);
            } else {
                std::vector<std::shared_ptr<std::thread> > threads;
                for ( int i=0; i<5; ++i) {
                    shared_ptr<thread> t( new thread( boost::bind( &boost::asio::io_service::run, &ioservice ) ) );
                    threads.push_back( t );
                }
                int priority = vm["priority"].as<int>();
                shared_ptr<thread> t( new thread( boost::bind( uploadJobs, conn, jobs, priority, std::ref(logger)) ) );
                threads.push_back( t );
                //thread t( boost::bind( &boost::asio::io_service::run, &ioservice ) );
                //thread tt( boost::bind( &boost::asio::io_service::run, &ioservice ) );
                //uploadJobs(conn, jobs, logger);
                //ioservice.run();
                //t.join();
                //tt.join();
                for( auto & it : threads ) {
                    it->join();
                }

            }
        }

    }
    catch( const exception &e ) {
        cerr << "Uncaught exception (fatal): " << e.what() << endl;
    }

    return EXIT_SUCCESS;

}

