#ifndef REDUX_MOMFBD_OBJECT_HPP
#define REDUX_MOMFBD_OBJECT_HPP

#include "redux/momfbd/channel.hpp"
#include "redux/util/array.hpp"
#include "redux/types.hpp"

#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/thread.hpp>

namespace po = boost::program_options;
namespace bpt = boost::property_tree;


namespace redux {

    namespace momfbd {

        /*! @ingroup momfbd
         *  @{
         */

        class MomfbdJob;
        
        /*! @brief Class containing the object-specific configuration a MOMFBD job
         * 
         */
        class Object {

        public:

            Object( const MomfbdJob& );
            ~Object();

            void parseProperties( bpt::ptree& tree, const std::string& fn );
            bpt::ptree getPropertyTree( bpt::ptree* root=nullptr );

            size_t size(void) const;
            char* pack(char*) const;
            const char* unpack(const char*, bool);
        
            std::vector<uint32_t> imageNumbers, sequenceNumbers, darkNumbers;
            double reg_gamma, weight, angle, lambda;
            uint32_t nPoints, sequenceNumber, nph;
            std::vector<double> stokesWeights;

        private:

            void loadData(boost::asio::io_service&, boost::thread_group&);
            void preprocessData(boost::asio::io_service&, boost::thread_group&);
            bool isValid(void);
            
            Point clipImages(void);
            
            uint32_t flags;

            std::vector<std::shared_ptr<Channel>> channels;
            std::vector<uint32_t> wf_num;
            std::string imageDataDir, outputFileName;
            uint8_t fillpix_method, output_data_type;
            double lim_freq, r_c;
            const MomfbdJob& myJob;
            redux::util::Array<double> pupil;

            friend class MomfbdJob;
            friend class Channel;

        };


        /*! @} */
        
    }

}

#endif  // REDUX_MOMFBD_OBJECT_HPP
