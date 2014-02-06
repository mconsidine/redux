#ifndef REDUX_FILE_ANAINFO_HPP
#define REDUX_FILE_ANAINFO_HPP

#include "redux/util/file.hpp"
#include "redux/file/fileinfo.hpp"

#include <memory>

#define MAGIC_ANA           0x5555aaaa
#define MAGIC_ANAR          0xaaaa5555          // stored with wrong endianess

namespace redux {

    namespace file {
        
        struct AnaInfo : public redux::util::FileInfo {
            
            AnaInfo(void);
            AnaInfo(const std::string&);
       
            void read( redux::util::File& );
            void read( const std::string& );
            
            void write( redux::util::File& );
            
            std::string getText(void) {
                return m_Header.txt + m_ExtendedHeader;
            }
            
            struct raw_header {                    // first block for ana files
                uint32_t synch_pattern;
                uint8_t subf;
                uint8_t source;
                uint8_t nhb;
                uint8_t datyp;
                uint8_t ndim;
                uint8_t free1;
                uint8_t cbytes[4];
                uint8_t free[178];
                uint32_t dim[16];
                char txt[256];
            } m_Header;

            struct compressed_header {
                uint32_t tsize, nblocks, bsize;
                uint8_t slice_size, type;
            } m_CompressedHeader;

            std::string m_ExtendedHeader;
            size_t hdrSize;
            
        };

        std::shared_ptr<AnaInfo> readAnaInfo( const std::string& );
        
    } // end namespace file

} // end namespace redux


#endif // REDUX_FILE_ANAINFO_HPP
