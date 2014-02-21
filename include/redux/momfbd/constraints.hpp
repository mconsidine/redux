#ifndef REDUX_MOMFBD_CONSTRAINTS_HPP
#define REDUX_MOMFBD_CONSTRAINTS_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>

namespace po = boost::program_options;
namespace bpt = boost::property_tree;


namespace redux {

    namespace momfbd {

        /*! @ingroup momfbd
         *  @{
         */

        class Constraints {

            struct cstrvar {
                uint32_t *o_map, *i_map;
                uint32_t nk, m;
                double *k;
                bool operator<(const cstrvar& rhs) const { return (m<rhs.m); };
            };

            // actual nullspace storage
            double ***rcs;        // reduced constraints
            //double ***groups;        // groups
            std::vector<std::shared_ptr<double*>> groups;
            uint32_t nGroups;           // number of groups
            std::vector<uint32_t> nVerticals, nHorizontals;     // vertical lines/group, horizontal elements/line
            uint32_t n_imap, n_omap;
            std::vector<uint32_t> imap_n, omap_n;
            std::vector<std::vector<uint32_t>> i_map, o_map;
            // references to nullspace lines and maps
            uint32_t nrv;
            
        public:
            std::vector<cstrvar> rv;

            Constraints();
            ~Constraints();

            size_t size(void) const;
            char* pack( char* ) const;
            const char* unpack( const char*, bool );

            void sort(void) { std::sort(rv.begin(),rv.end()); };

        };

        /*! @} */

    }   // momfbd

}   // redux

#endif  // REDUX_MOMFBD_CONSTRAINTS_HPP
