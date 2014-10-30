#include "redux/momfbd/channel.hpp"

#include "redux/momfbd/defines.hpp"
#include "redux/momfbd/momfbdjob.hpp"
#include "redux/momfbd/object.hpp"

#include "redux/constants.hpp"
#include "redux/file/fileana.hpp"
#include "redux/file/fileio.hpp"
#include "redux/math/functions.hpp"
#include "redux/image/utils.hpp"
#include "redux/logger.hpp"
#include "redux/translators.hpp"
#include "redux/util/stringutil.hpp"

#include <math.h>
#include <string>

#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>

#include "redux/image/fouriertransform.hpp"

namespace bfs = boost::filesystem;
using namespace redux::image;
using namespace redux::momfbd;
using namespace redux::util;
using namespace redux;
using namespace std;

#define lg Logger::mlg
namespace {

    const string thisChannel = "momfbdch";

    void parseSegment( vector<uint32_t>& divs, vector<uint32_t>& types, const string& elem ) {
        size_t n = std::count( elem.begin(), elem.end(), '-' );
        uint32_t tp = CFG_DEFAULT;
        if( elem.find_first_of( "Zz" ) != string::npos ) tp = CFG_ZERNIKE;
        if( elem.find_first_of( "Kk" ) != string::npos ) {
            if( tp == CFG_DEFAULT ) {
                tp = CFG_KARHUNEN_LOEVE;
            }
            else {
                LOG_CRITICAL << "error: different types in specified mode range \"" << elem << "\"";
            }
        }
        string tmp = elem;
        tmp.erase( boost::remove_if( tmp, boost::is_any_of( "ZzKk" ) ), tmp.end() );
        if( n == 0 ) {
            divs.push_back( boost::lexical_cast<uint32_t>( tmp ) );
            types.push_back( tp );
            return;
        }
        else if( n == 1 ) {
            n = tmp.find_first_of( '-' );
            uint32_t first = boost::lexical_cast<uint32_t>( tmp.substr( 0, n ) );
            uint32_t last = boost::lexical_cast<uint32_t>( tmp.substr( n + 1 ) );
            while( first <= last ) {
                divs.push_back( first++ );
                types.push_back( tp );
            }
        }
    }

    double def2cf( double pd_defocus, double telescope_r ) { // defocus distance in meters
        static const double tmp = ( 8.0 * sqrt( 3.0 ) );
        return -pd_defocus * redux::PI * telescope_r * telescope_r * tmp;
    }

}


Channel::Channel( const Object& o, const MomfbdJob& j ) : flags( o.flags ), mmRow( 0 ), mmWidth( 0 ), fillpix_method( o.fillpix_method ),
    image_num_offs( 0 ), sequenceNumber( o.sequenceNumber ), nf( 0 ), myObject( o ), myJob( j ) {

}

Channel::~Channel() {

}

void Channel::parseProperties( bpt::ptree& tree ) {

    LOG_DEBUG << "Channel::parseProperties()";

    imageNumbers = tree.get<vector<uint32_t>>( "IMAGE_NUM", myObject.imageNumbers );

    if( imageNumbers.size() == 0 ) LOG_CRITICAL << "Still no image sequence numbers at channel level.";
    sequenceNumber = tree.get<uint32_t>( "SEQUENCE_NUM", myObject.sequenceNumber );
    darkNumbers = tree.get<vector<uint32_t>>( "DARK_NUM", myObject.darkNumbers );

    alignClip = tree.get<vector<int16_t>>( "ALIGN_CLIP", vector<int16_t>() );
    if( !alignClip.empty() && alignClip.size() != 4 ) {
        LOG_ERR << "argument to ALIGN_CLIP could not be translated to 4 integers. Whole image area will be used !!";
        alignClip.clear();
    }
    wf_num = tree.get<vector<uint32_t>>( "WFINDEX", myObject.wf_num );

    imageDataDir = cleanPath( tree.get<string>( "IMAGE_DATA_DIR", myJob.imageDataDir ) );
    if( imageDataDir.length() == 0 ) {
        imageDataDir = cleanPath( "./" );      // Nothing specified in cfg file, use current directory.
    }

    imageTemplate = cleanPath( tree.get<string>( "FILENAME_TEMPLATE", "" ) );
    if( imageTemplate.length() == 0 ) {
        LOG_ERR << "no filename template specified.";
    }

    darkTemplate = tree.get<string>( "DARK_TEMPLATE", "" );
    if( darkTemplate.length() > 0 ) {
        if( darkNumbers.size() == 0 ) {
            LOG_ERR << "darkfield template specified but no dark numbers.";
        }
    }
    else if( darkNumbers.size() > 0 ) {
        LOG_ERR << "darkfield dark numbers specified but no darkfield template.";
    }

    gainFile = tree.get<string>( "GAIN_FILE", "" );
    if( gainFile.length() > 0 ) {
        if( darkTemplate.length() == 0 ) {
            LOG_ERR << "a gain file name but no dark field was specified.";
        }
    }
    else if( darkTemplate.length() > 0 ) {
        LOG_ERR << "a dark field name but no gain file was specified.";
    }

    responseFile = tree.get<string>( "CCD_RESPONSE", "" );
    if( ( responseFile.length() > 0 ) && ( gainFile.length() == 0 ) ) {
        LOG_ERR << "detector response correction only possible when flatfielding.";
    }

    backgainFile = cleanPath( tree.get<string>( "BACK_GAIN", "" ), imageDataDir );
    psfFile = cleanPath( tree.get<string>( "PSF", "" ), imageDataDir );
    mmFile = cleanPath( tree.get<string>( "MODMAT", "" ), imageDataDir );

    if( mmFile.length() > 0 ) {
        mmRow = tree.get<uint8_t>( "MMROW", 0 );
        if( tree.get<bool>( "MMROW", false ) ) {
            LOG_CRITICAL << "a modulation matrix was provided but no row specified (MMROW).";
        }
        mmWidth = tree.get<uint8_t>( "MMWIDTH", 0 );
        if( tree.get<bool>( "MMWIDTH", false ) ) {
            LOG_CRITICAL << "modulation matrix dimensions cannot be autodetected (yet): you must provide the matrix width (MMWIDTH)!";
        }
        stokesWeights = tree.get<vector<double>>( "VECTOR", myObject.stokesWeights );
        if( stokesWeights.size() > 0 ) {
            if( stokesWeights.size() != mmWidth ) {
                LOG_ERR << "VECTOR input has " << stokesWeights.size() << " elements, but MMWIDTH=" << mmWidth;
            }
        }
        else {
            stokesWeights = myObject.stokesWeights;
            if( stokesWeights.size() == 0 ) {
                LOG_ERR << "modulation matrix specified but no VECTOR input given!";
            }
        }
    }
    else {
        mmRow = mmWidth = 1;
        stokesWeights.resize( 1, 1.0 );
    }

    offxFile = tree.get<string>( "XOFFSET", "" );
    offyFile = tree.get<string>( "YOFFSET", "" );

    string tmpString = tree.get<string>( "DIVERSITY", "" );
    if( tmpString.size() == 0 ) {
        LOG_WARN << "no diversity specified (assuming zero).";
        diversity.resize( 1, 0.0 );
        diversityOrders.resize( 1, 4 );
        diversityTypes.resize( 1, CFG_ZERNIKE );
    }
    else {
        double tmpD;
        if( tmpString.find( "mm" ) != string::npos ) tmpD = 1.00E-03;
        else if( tmpString.find( "cm" ) != string::npos ) tmpD = 1.00E-02;
        else tmpD = 1.0;
        tmpString.erase( boost::remove_if( tmpString, boost::is_any_of( "cm\" " ) ), tmpString.end() );
        bpt::ptree tmpTree;                         // just to be able to use the VectorTranslator
        tmpTree.put( "tmp", tmpString );
        diversity = tmpTree.get<vector<double>>( "tmp", vector<double>() );
        tmpString = tree.get<string>( "DIV_ORDERS", "" );
        if( tmpString.empty() ) {
            if( diversity.size() > 1 ) {
                LOG_ERR << "multiple coefficients found but no diversity orders specified!";
            }
            else {
                diversityOrders.resize( 1, 4 );
                diversityTypes.resize( 1, CFG_ZERNIKE );
                diversity[1] = def2cf( tmpD * diversity[1], myJob.telescopeDiameter / myJob.telescopeFocalLength );
            }
        }
        else {
            vector<string> tmp;
            boost::split( tmp, tmpString, boost::is_any_of( "," ) );
            for( auto & it : tmp ) {
                parseSegment( diversityOrders, diversityTypes, it );
            }
            if( diversity.size() != diversityOrders.size() ) {
                LOG_ERR << "number of diversity orders does not match number of diversity coefficients!";
            }
        }
    }

    fillpix_method = MomfbdJob::getFromMap( tmpString = tree.get<string>( "FPMETHOD", "" ), fillpixMap );
    if( fillpix_method == 0 ) {
        if( tmpString.length() > 0 ) {
            LOG_ERR << "unknown fillpix method \"" << tmpString << "\"\n  Valid entries currently are: "
                    "\"median\", \"invdistweight\" or \"horint\"";
        }
        fillpix_method = myObject.fillpix_method;
    }

    nf = tree.get<double>( "NF", DEF_NF );
    image_num_offs = tree.get<int>( "DT", 0 );

    flags = myObject.flags;
    MomfbdJob::maybeOverride( tree.get<bool>( "NO_RESTORE", flags & MFBD_NO_RESTORE ), flags, MFBD_NO_RESTORE );
    MomfbdJob::maybeOverride( tree.get<bool>( "SAVE_FFDATA", flags & MFBD_SAVE_FFDATA ), flags, MFBD_SAVE_FFDATA );

    size_t p;
    if( ( p = imageTemplate.find_first_of( '%' ) ) != string::npos ) {
        if( sequenceNumber > 0 ) {
            size_t q;
            if( ( q = imageTemplate.find_first_of( '%', p + 1 ) ) != string::npos ) {
                tmpString = boost::str( boost::format( imageTemplate.substr( 0, q ) ) % sequenceNumber );
                imageTemplate = tmpString + imageTemplate.substr( q );
            }
            else  LOG_WARN << boost::format( "file name template \"%s\" does not contain a 2nd format specifier (needs 2)" ) % imageTemplate;
        }
    }
    else {
        LOG_WARN << boost::format( "file name template \"%s\" does not contain a format specifier (needs %d)" ) % imageTemplate % ( 1 + ( sequenceNumber >= 0 ) );
    }

    if( tree.get<bool>( "INCOMPLETE", false ) ) {
        for( size_t i( 0 ); i < imageNumbers.size(); ) {
            bfs::path fn = bfs::path( imageDataDir ) / bfs::path( boost::str( boost::format( imageTemplate ) % ( image_num_offs + imageNumbers[i] ) ) );
            if( !bfs::exists( fn ) ) {
                imageNumbers.erase( imageNumbers.begin() + i );
                continue;
            }
            ++i;
        }
        if( imageNumbers.empty() ) {
            LOG_CRITICAL << boost::format( "no files found for incomplete object with filename template \"%s\" in directory \"%s\"" ) % imageTemplate % imageDataDir;
        }
    }
    else {
        for( size_t i( 0 ); i < imageNumbers.size(); ) {
            bfs::path fn = bfs::path( imageDataDir ) / bfs::path( boost::str( boost::format( imageTemplate ) % ( image_num_offs + imageNumbers[i] ) ) );
            if( !bfs::exists( fn ) ) {
                LOG_CRITICAL << boost::format( "file %s not found!" ) % fn;
            }
            ++i;
        }

    }


    LOG_DEBUG << "Channel::parseProperties() done.";

}



bpt::ptree Channel::getPropertyTree( bpt::ptree* root ) {

    bpt::ptree tree;

    if( imageNumbers != myObject.imageNumbers ) tree.put( "IMAGE_NUM", imageNumbers );
    if( sequenceNumber != myObject.sequenceNumber ) tree.put( "SEQUENCE_NUM", sequenceNumber );
    if( darkNumbers != myObject.darkNumbers ) tree.put( "DARK_NUM", darkNumbers );
    if( alignClip.size() == 4 ) tree.put( "ALIGN_CLIP", alignClip );
    if( wf_num != myObject.wf_num ) tree.put( "WFINDEX", wf_num );
    if( imageDataDir != myObject.imageDataDir ) tree.put( "IMAGE_DATA_DIR", imageDataDir );
    if( !imageTemplate.empty() ) tree.put( "FILENAME_TEMPLATE", imageTemplate );
    if( !darkTemplate.empty() ) tree.put( "DARK_TEMPLATE", darkTemplate );
    if( !gainFile.empty() ) tree.put( "GAIN_FILE", gainFile );
    if( !responseFile.empty() ) tree.put( "CCD_RESPONSE", responseFile );
    if( !backgainFile.empty() ) tree.put( "BACK_GAIN", backgainFile );
    if( !psfFile.empty() ) tree.put( "PSF", psfFile );
    if( !mmFile.empty() ) tree.put( "MODMAT", mmFile );
    if( mmRow ) tree.put( "MMROW", mmRow );
    if( mmWidth ) tree.put( "MMWIDTH", mmWidth );
    if( stokesWeights != myObject.stokesWeights ) tree.put( "VECTOR", stokesWeights );
    if( !offxFile.empty() ) tree.put( "XOFFSET", offxFile );
    if( !offyFile.empty() ) tree.put( "YOFFSET", offyFile );
    if( !diversity.empty() ) tree.put( "DIVERSITY", diversity );
    if( !diversityOrders.empty() ) tree.put( "DIV_ORDERS", diversityOrders ); // TODO types missing
    if( fillpix_method != myObject.fillpix_method ) tree.put( "FPMETHOD", fillpix_method );
    if( nf != DEF_NF ) tree.put( "NF", nf );
    if( image_num_offs != 0 ) tree.put( "DT", image_num_offs );
    uint32_t dflags = flags ^ myObject.flags;
    if( dflags & MFBD_NO_RESTORE ) tree.put( "NO_RESTORE", ( bool )( flags & MFBD_NO_RESTORE ) );
    if( dflags & MFBD_SAVE_FFDATA ) tree.put( "SAVE_FFDATA", ( bool )( flags & MFBD_SAVE_FFDATA ) );
    // TODO "INCOMPLETE"

    if( root ) {
        root->push_back( bpt::ptree::value_type( "channel", tree ) );
    }

    return tree;

}


size_t Channel::size( void ) const {

    size_t sz = 3;
    sz += 3 * sizeof( uint32_t );
    sz += sizeof( double );
    sz += imageNumbers.size() * sizeof( uint32_t ) + sizeof( size_t );
    sz += darkNumbers.size() * sizeof( uint32_t ) + sizeof( size_t );
    sz += alignClip.size() * sizeof( int16_t ) + sizeof( size_t );
    sz += wf_num.size() * sizeof( uint32_t ) + sizeof( size_t );
    sz += stokesWeights.size() * sizeof( double ) + sizeof( size_t );
    sz += diversity.size() * sizeof( double ) + sizeof( size_t );
    sz += diversityOrders.size() * sizeof( uint32_t ) + sizeof( size_t );
    sz += diversityTypes.size() * sizeof( uint32_t ) + sizeof( size_t );
    sz += imageDataDir.length() + imageTemplate.length() + darkTemplate.length() + gainFile.length() + 4;
    sz += responseFile.length() + backgainFile.length() + psfFile.length() + mmFile.length() + 4;
    sz += offxFile.length() + offyFile.length() + 2;
    sz += dark.size();

    return sz;
}


char* Channel::pack( char* ptr ) const {
    using redux::util::pack;

    ptr = pack( ptr, fillpix_method );
    ptr = pack( ptr, mmRow );
    ptr = pack( ptr, mmWidth );
    ptr = pack( ptr, sequenceNumber );
    ptr = pack( ptr, image_num_offs );
    ptr = pack( ptr, flags );
    ptr = pack( ptr, nf );
    ptr = pack( ptr, imageNumbers );
    ptr = pack( ptr, darkNumbers );
    ptr = pack( ptr, alignClip );
    ptr = pack( ptr, wf_num );
    ptr = pack( ptr, stokesWeights );
    ptr = pack( ptr, diversity );
    ptr = pack( ptr, diversityOrders );
    ptr = pack( ptr, diversityTypes );
    ptr = pack( ptr, imageDataDir );
    ptr = pack( ptr, imageTemplate );
    ptr = pack( ptr, darkTemplate );
    ptr = pack( ptr, gainFile );
    ptr = pack( ptr, responseFile );
    ptr = pack( ptr, backgainFile );
    ptr = pack( ptr, psfFile );
    ptr = pack( ptr, mmFile );
    ptr = pack( ptr, offxFile );
    ptr = pack( ptr, offyFile );

    ptr = dark.pack( ptr );

    return ptr;
}


const char* Channel::unpack( const char* ptr, bool swap_endian ) {
    using redux::util::unpack;

    ptr = unpack( ptr, fillpix_method );
    ptr = unpack( ptr, mmRow );
    ptr = unpack( ptr, mmWidth );
    ptr = unpack( ptr, sequenceNumber, swap_endian );
    ptr = unpack( ptr, image_num_offs, swap_endian );
    ptr = unpack( ptr, flags, swap_endian );
    ptr = unpack( ptr, nf, swap_endian );
    ptr = unpack( ptr, imageNumbers, swap_endian );
    ptr = unpack( ptr, darkNumbers, swap_endian );
    ptr = unpack( ptr, alignClip, swap_endian );
    ptr = unpack( ptr, wf_num, swap_endian );
    ptr = unpack( ptr, stokesWeights, swap_endian );
    ptr = unpack( ptr, diversity, swap_endian );
    ptr = unpack( ptr, diversityOrders, swap_endian );
    ptr = unpack( ptr, diversityTypes, swap_endian );
    ptr = unpack( ptr, imageDataDir );
    ptr = unpack( ptr, imageTemplate );
    ptr = unpack( ptr, darkTemplate );
    ptr = unpack( ptr, gainFile );
    ptr = unpack( ptr, responseFile );
    ptr = unpack( ptr, backgainFile );
    ptr = unpack( ptr, psfFile );
    ptr = unpack( ptr, mmFile );
    ptr = unpack( ptr, offxFile );
    ptr = unpack( ptr, offyFile );

    ptr = dark.unpack( ptr, swap_endian );

    return ptr;
}


bool Channel::isValid( void ) {

    bool allOk( true );

    if( imageTemplate.empty() ) {
        LOG_ERR << "No images specified.";
    }
    else {
        if( imageNumbers.empty() ) {
            bfs::path fn = bfs::path( imageDataDir ) / bfs::path( imageTemplate );
            if( ! bfs::exists( fn ) ) {
                LOG_ERR << boost::format( "file %s not found!" ) % fn;
                allOk = false;
            }
        }
        else {
            for( auto & it : imageNumbers ) {
                bfs::path fn = bfs::path( imageDataDir ) / bfs::path( boost::str( boost::format( imageTemplate ) % it ) );
                if( !bfs::exists( fn ) ) {
                    LOG_ERR << boost::format( "file %s not found!" ) % fn;
                    allOk = false;
                }
            }
        }
    }

    if( !darkTemplate.empty() ) {
        if( darkNumbers.empty() ) {
            bfs::path fn = bfs::path( imageDataDir ) / bfs::path( darkTemplate );
            if( ! bfs::exists( fn ) ) {
                LOG_ERR << boost::format( "file %s not found!" ) % fn;
                allOk = false;
            }
        }
        else {
            for( auto & it : darkNumbers ) {
                bfs::path fn = bfs::path( imageDataDir ) / bfs::path( boost::str( boost::format( darkTemplate ) % it ) );
                if( !bfs::exists( fn ) ) {
                    LOG_ERR << boost::format( "file %s not found!" ) % fn;
                    allOk = false;
                }
            }
        }
    }

    if( !gainFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( gainFile );
        if( ! bfs::exists( fn ) ) {
            LOG_ERR << boost::format( "file %s not found!" ) % fn;
            allOk = false;
        }
    }

    if( !responseFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( responseFile );
        if( ! bfs::exists( fn ) ) {
            LOG_ERR << boost::format( "file %s not found!" ) % fn;
            allOk = false;
        }
    }

    if( !backgainFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( backgainFile );
        if( ! bfs::exists( fn ) ) {
            LOG_ERR << boost::format( "file %s not found!" ) % fn;
            allOk = false;
        }
    }

    if( !psfFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( psfFile );
        if( ! bfs::exists( fn ) ) {
            LOG_ERR << boost::format( "file %s not found!" ) % fn;
            allOk = false;
        }
    }

    if( !mmFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( mmFile );
        if( ! bfs::exists( fn ) ) {
            LOG_ERR << boost::format( "file %s not found!" ) % fn;
            allOk = false;
        }
    }

    if( !offxFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( offxFile );
        if( ! bfs::exists( fn ) ) {
            LOG_ERR << boost::format( "file %s not found!" ) % fn;
            allOk = false;
        }
    }

    if( !offyFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( offyFile );
        if( ! bfs::exists( fn ) ) {
            LOG_ERR << boost::format( "file %s not found!" ) % fn;
            allOk = false;
        }
    }


    return allOk;

}


void Channel::loadData( boost::asio::io_service& service, boost::thread_group& pool ) {

    // TODO: absolute/relative paths
    // TODO: cache files and just fetch shared_ptr

    if( !darkTemplate.empty() ) {
        if( darkNumbers.empty() ) {
            bfs::path fn = bfs::path( imageDataDir ) / bfs::path( darkTemplate );
            LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
            redux::file::readFile( fn.string(), dark );
            dark.normalize();
        }
        else {
            Image<float> tmp;
            for( size_t di = 0; di < darkNumbers.size(); ++di ) {
                bfs::path fn = bfs::path( imageDataDir ) / bfs::path( boost::str( boost::format( darkTemplate ) % darkNumbers[di] ) );
                LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
                if( !di ) {
                    redux::file::readFile( fn.string(), dark );
                }
                else {
                    redux::file::readFile( fn.string(), tmp );
                    dark += tmp;
                }
            }
            dark.normalize();
        }
    }


    if( !gainFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( gainFile );
        LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
        redux::file::readFile( fn.string(), gain );
    }

    if( !responseFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( responseFile );
        LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
        redux::file::readFile( fn.string(), ccdResponse );
    }

    if( !backgainFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( backgainFile );
        LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
        redux::file::readFile( fn.string(), ccdScattering );
    }

    if( !psfFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( psfFile );
        LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
        redux::file::readFile( fn.string(), psf );
    }

    if( !mmFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( mmFile );
        LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
        redux::file::readFile( fn.string(), modulationMatrix );
    }

    if( !offxFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( offxFile );
        LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
        redux::file::readFile( fn.string(), xOffset );
    }

    if( !offyFile.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( offyFile );
        LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
        redux::file::readFile( fn.string(), yOffset );
    }


    if( imageNumbers.empty() ) {
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( imageTemplate );
        LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
        redux::file::readFile( fn.string(), images );
    }
    else {
        Image<float> tmp;
        size_t nImages = imageNumbers.size();
        bfs::path fn = bfs::path( imageDataDir ) / bfs::path( boost::str( boost::format( imageTemplate ) % imageNumbers[0] ) );
        redux::file::readFile( fn.string(), tmp );
        imageStats.resize( nImages );
        images.resize( nImages, tmp.dimSize( 0 ), tmp.dimSize( 1 ) );

        for( size_t i = 0; i < nImages; ++i ) {
            service.post( boost::bind( &Channel::loadImage, this, i ) );

        }

    }


}


void Channel::preprocessData( boost::asio::io_service& service, boost::thread_group& pool ) {

    size_t nImages = imageNumbers.size();
    double avgMean = 0.0;
    for( size_t i = 0; i < nImages; ++i ) {
        avgMean += imageStats[i]->mean;
    }
    avgMean /= static_cast<double>( nImages );

    for( size_t i = 0; i < nImages; ++i ) {
        service.post( boost::bind( &Channel::preprocessImage, this, i, avgMean ) );
    }

    srand(time(0));
}


double Channel::getMaxMean(void) {
    double maxMean = std::numeric_limits<double>::min();
    for(auto it: imageStats ) {
        if( it->mean > maxMean ) maxMean = it->mean;
    }
    return maxMean;
}


void Channel::normalizeData(boost::asio::io_service& service, boost::thread_group& pool, double value) {
    size_t nImages = imageNumbers.size();
    for( size_t i = 0; i < nImages; ++i ) {
        service.post( boost::bind( &Channel::normalizeImage, this, i, value ) );
    }
}


void Channel::loadImage( size_t index ) {
    Image<float> subimg( images, index, index, 0, images.dimSize( 1 ) - 1, 0, images.dimSize( 2 ) - 1 );
    bfs::path fn = bfs::path( imageDataDir ) / bfs::path( boost::str( boost::format( imageTemplate ) % imageNumbers[index] ) );
    LOG_DETAIL << boost::format( "Loading file %s" ) % fn;
    redux::file::readFile( fn.string(), subimg );
    imageStats[index].reset(new Statistics<float>());
    imageStats[index]->getStats(myJob.borderClip,subimg,ST_VALUES);                          // only get min/max/mean
}


void Channel::preprocessImage( size_t index, double avgMean ) {

    double& mean = imageStats[index]->mean;
    bool modified = false;

    Image<float> subimg( images, index, index, 0, images.dimSize( 1 ) - 1, 0, images.dimSize( 2 ) - 1 );
    bfs::path fn = bfs::path( boost::str( boost::format( imageTemplate ) % imageNumbers[index] ) );
    LOG_DETAIL << boost::format( "Pre-processing image %s" ) % fn;
    
    // Michiel's method for detecting bitshifted Sarnoff images.
    if( mean > 4 * avgMean ) {
        LOG_WARN << boost::format( "Image bit shift detected for image %d (mean > 4*avgMean). adjust factor=0.625 (keep your fingers crossed)!" ) % index;
        subimg *= 0.625;
        modified = true;
    }
    else if( mean < 0.25 * avgMean ) {
        LOG_WARN << boost::format( "Image bit shift detected for image %d (mean < 0.25*avgMean). adjust factor=16 (keep your fingers crossed)!" ) % index;
        subimg *= 16;
        modified = true;
    }

    LOG_WARN << boost::format( "Dimensions of simg (%s)." ) % printArray( subimg.dimensions(), "" );
    LOG_WARN << boost::format( "Dimensions of dark (%s)." ) % printArray( dark.dimensions(), "" );
    LOG_WARN << boost::format( "Dimensions of gain (%s)." ) % printArray( gain.dimensions(), "" );

    if( dark.valid() && gain.valid() ) {
        if( ! subimg.sameSize( dark ) ) {
            LOG_ERR << boost::format( "Dimensions of dark (%s) does not match this image (%s), skipping flatfielding !!" ) % printArray( dark.dimensions(), "" ) % printArray( subimg.dimensions(), "" );
            return;
        }
        if( ! subimg.sameSize( gain ) ) {
            LOG_ERR << boost::format( "Dimensions of gain (%s) does not match this image (%s), skipping flatfielding !!" ) % printArray( gain.dimensions(), "" ) % printArray( subimg.dimensions(), "" );
            return;
        }
        if( ccdResponse.valid() && !subimg.sameSize( ccdResponse ) ) {
            LOG_WARN << boost::format( "Dimensions of ccd-response (%s) does not match this image (%s), will not be used !!" ) % printArray( ccdResponse.dimensions(), "" ) % printArray( subimg.dimensions(), "" );
            ccdResponse.resize();
        }

        subimg -= dark;
        modified = true;

        if( ccdResponse.valid() ) {  // correct for the detector response (this should not contain the gain correction and must be done before descattering)
            subimg *= ccdResponse;
        }

        if( ccdScattering.valid() && psf.valid() ) {          // apply backscatter correction
            if( subimg.sameSize( ccdScattering ) && subimg.sameSize( psf ) ) {
                LOG_DETAIL << "Applying correction for CCD transparency.";
                // TODO:  descatter(data,psf,bgain,io);
            }
            else {
                LOG_ERR << boost::format( "Dimensions of ccdScattering (%s) or psf (%s) does not match this image (%s), skipping flatfielding !!" )
                        % printArray( ccdScattering.dimensions(), "" ) % printArray( psf.dimensions(), "" ) % printArray( subimg.dimensions(), "" );
            }
        }

        subimg *= gain;
        namespace sp = std::placeholders;
        size_t ni = images.dimSize(0);
        size_t sy = images.dimSize(1);
        size_t sx = images.dimSize(2);
        shared_ptr<float**> array = images.get(ni,sy,sx);
        float*** arrayPtr = array.get();
        float badPixelThreshold = 1E-5;     // TODO: configurable threshold.
        switch(fillpix_method) {
            case CFG_FPM_HORINT: {
                function<float(size_t,size_t)> func = bind(horizontalInterpolation<float>, arrayPtr[index], sy, sx, sp::_1, sp::_2);
                fillPixels(arrayPtr[index], sy, sx, func, std::bind2nd(std::less_equal<float>(),badPixelThreshold) );
                break;
            }
            case CFG_FPM_MEDIAN: {
                // TODO: median method
                break;
            }
            case CFG_FPM_INVDISTWEIGHT:       // inverse distance weighting is the default method, so fall through
            default: {
                function<double(size_t,size_t)> func = bind(inverseDistanceWeight<float>, arrayPtr[index], sy, sx, sp::_1, sp::_2);
                //function<double(size_t,size_t)> func = bind(inv_dist_wght, arrayPtr[index], sy, sx, sp::_1, sp::_2);
                fillPixels(arrayPtr[index], sy, sx, func, std::bind2nd(std::less_equal<float>(),badPixelThreshold) );
            }
        }

    }

    imageStats[index]->getStats(myJob.borderClip, subimg);      // get all stats for the cleaned up data

    if( modified && flags & MFBD_SAVE_FFDATA ) {
        //string fnT = fn.leaf().string();
        fn = bfs::path( fn.leaf().string() + ".cor" );

        LOG_DETAIL << boost::format( "Saving file \"%s\"" ) % fn.string();
        redux::file::Ana::write( fn.string(), subimg );
        
      /*  
        //FourierTransform bla(subimg);
        Image<float> apod = subimg.copy();
        apod.trim();
        cout << printArray(apod.dimensions(),"blaDims") << endl;
        apodize(apod,45);
        //subimg *= apod;
        Image<double> psf(subimg.dimensions(true));
        size_t sy = psf.dimSize(0);
        size_t sx = psf.dimSize(1);
        cout << printArray(psf.dimensions(),"psfDims") << endl;
        redux::math::gauss(psf.ptr(),sy,sx,5,5,sy/2,sx/2);
        //FourierTransform::reorder(psf);
        //redux::file::Ana::write( fnT+".rpsf", psf );
        FourierTransform psft(psf, FT_REORDER|FT_NORMALIZE);
        //blat.reorder();
        auto conv = psft.convolve(subimg);
        cout << printArray(conv.dimensions(),"convDims") << endl;
        Image<float> power(psft.dimensions());
        auto rit = psft.begin();
        for(auto& it: power) {
           //it = rit++->real();
           it = std::abs(*rit++);
        }
        //bla.copy(bla2);
        redux::file::Ana::write( fnT+".psf", psf );
        redux::file::Ana::write( fnT+".pwr", power );
        redux::file::Ana::write( fnT+".apod", apod );
        redux::file::Ana::write( fnT+".conv", conv );
        */
        
    }



}


void Channel::normalizeImage( size_t index, double value ) {
    Image<float> subimg( images, index, index, 0, images.dimSize( 1 ) - 1, 0, images.dimSize( 2 ) - 1 );
    LOG_DETAIL << boost::format( "Normalizing image %d" ) % index;
    subimg *= (value/imageStats[index]->mean);
}


size_t Channel::sizeOfPatch(uint32_t npixels) const {
    size_t sz = sizeof(size_t) + imageStats.size()*sizeof(float);
    sz += npixels*images.dimSize(0)*sizeof(float);
    return sz;
}


char* Channel::packPatch( Patch::Ptr patch, char* ptr ) const {
    
    int localTiltX = 0;
    int localTiltY = 0;
    if(xOffset.valid()) {
        Image<int16_t> tmpOff(xOffset, patch->first.y, patch->last.y, patch->first.x, patch->last.x);
        Statistics<int16_t> stats;
        stats.getStats(tmpOff,ST_VALUES);
        double tmp;
        patch->residualTilts.x = modf(stats.mean/100 , &tmp);
        localTiltX = lrint(tmp);
    }
    if(yOffset.valid()) {
        Image<int16_t> tmpOff(xOffset, patch->first.y, patch->last.y, patch->first.x, patch->last.x);
        Statistics<int16_t> stats;
        stats.getStats(Image<int16_t>(yOffset, patch->first.y, patch->last.y, patch->first.x, patch->last.x),ST_VALUES);
        double tmp;
        patch->residualTilts.y = modf(stats.mean/100 , &tmp);
        localTiltY = lrint(tmp);
    }

    vector<float> noise;
    for(auto it: imageStats) noise.push_back(it->noisePower);
    ptr = redux::util::pack( ptr, noise );
    
    Image<float> tmp = Image<float>( images, 0, images.dimSize(0),
                                     patch->first.y+localTiltY, patch->last.y+localTiltY,
                                     patch->first.x+localTiltX, patch->last.x+localTiltX ).copy();
    ptr = redux::util::pack( ptr, tmp.ptr(), tmp.nElements() );
    
    
    return ptr;
}
 

Point Channel::clipImages(void) {

    if( alignClip.size() != 4 ) {
        LOG_WARN << "No (or faulty) align-clip supplied, using full images: " << printArray(alignClip,"clip"); 
        return Point(images.dimSize(1),images.dimSize(2));
    }

    bool flipX=false,flipY=false;
    if( alignClip[0] > alignClip[1]) {     // we have the y (row) dimension first, momfbd cfg-files (and thus alignClip) has x first. TBD: how should this be ?
        std::swap(alignClip[0], alignClip[1]);
        flipX = true;
    }
    if( alignClip[2] > alignClip[3]) {
        std::swap(alignClip[2], alignClip[3]);
        flipY = true;
    }
    for( auto& it: alignClip ) --it;        // NOTE: momfbd uses 1-based indexes, we start with 0...
    LOG_DETAIL << "Clipping: " << printArray(alignClip,"alignClip");
    LOG_DETAIL << "Clipping image stack: " << printArray(images.dimensions(),"original size");
    images.setLimits( 0, imageNumbers.size()-1, alignClip[2], alignClip[3], alignClip[0], alignClip[1] );
    images.trim(false);
    
    if( dark.valid() ) {
        LOG_DETAIL << "Clipping dark: " << printArray(dark.dimensions(),"original size");
        dark.setLimits( alignClip[2], alignClip[3], alignClip[0], alignClip[1] );
        dark.trim();
    }

    if( gain.valid() ) {
        LOG_DETAIL << "Clipping gain: " << printArray(gain.dimensions(),"original size");
        gain.setLimits( alignClip[2], alignClip[3], alignClip[0], alignClip[1] );
        gain.trim();
    }

    if( ccdResponse.valid() ) {
        LOG_DETAIL << "Clipping ccdResponse: " << printArray(ccdResponse.dimensions(),"original size");
        ccdResponse.setLimits( alignClip[2], alignClip[3], alignClip[0], alignClip[1] );
        dark.trim();
    }

    if( ccdScattering.valid() ) {
        LOG_DETAIL << "Clipping ccdScattering: " << printArray(ccdScattering.dimensions(),"original size");
        ccdScattering.setLimits( alignClip[2], alignClip[3], alignClip[0], alignClip[1] );
        ccdScattering.trim();
    }

    if( psf.valid() ) {     // The PSF has to be symmetrically clipped, otherwise the convolution will be skewed.
        const std::vector<size_t>& dims = psf.dimensions();
        auto tmp = alignClip;
        size_t sy = alignClip[3] - alignClip[2] + 1;
        size_t sx = alignClip[1] - alignClip[0] + 1;
        if (dims.size() != 2 || dims[0] < sy || dims[1] < sx) throw std::logic_error("PSF has wrong dimensions: " + printArray(dims,"dims"));
        LOG_DETAIL << "Clipping psf: " << printArray(dims,"original size");
        int skewY = (dims[0] - sy)/2  - alignClip[2];
        int skewX = (dims[1] - sx)/2  - alignClip[0];
        tmp[0] += skewX;
        tmp[1] += skewX;
        tmp[2] += skewY;
        tmp[3] += skewY;
        LOG_DETAIL << "Clipping psf: " << printArray(tmp,"clip");
        psf.setLimits( tmp[2], tmp[3], tmp[0], tmp[1] );
        psf.trim();
    }

    if( flipX || flipY ) {
        LOG_DETAIL << "Flipping data.";
        size_t sy = alignClip[3] - alignClip[2] + 1;
        size_t sx = alignClip[1] - alignClip[0] + 1;
        size_t ni = imageNumbers.size();
        shared_ptr<float**> imgShared = images.get(ni,sy,sx);
        float*** imgPtr = imgShared.get();
        for(size_t i=0; i<ni; ++i) {
            if (flipX) reverseX(imgPtr[i],sy,sx);
            if (flipY) reverseY(imgPtr[i],sy,sx);
        }
        shared_ptr<float*> tmpShared;
        if( dark.valid() ) {
            tmpShared = dark.get(sy,sx);
            if (flipX) reverseX(tmpShared.get(),sy,sx);
            if (flipY) reverseY(tmpShared.get(),sy,sx);
        }

        if( gain.valid() ) {
            tmpShared = gain.get(sy,sx);
            if (flipX) reverseX(tmpShared.get(),sy,sx);
            if (flipY) reverseY(tmpShared.get(),sy,sx);
        }

        if( ccdResponse.valid() ) {
            tmpShared = ccdResponse.get(sy,sx);
            if (flipX) reverseX(tmpShared.get(),sy,sx);
            if (flipY) reverseY(tmpShared.get(),sy,sx);
        }

        if( ccdScattering.valid() ) {
            tmpShared = ccdScattering.get(sy,sx);
            if (flipX) reverseX(tmpShared.get(),sy,sx);
            if (flipY) reverseY(tmpShared.get(),sy,sx);
        }

        if( psf.valid() ) {
            tmpShared = psf.get(sy,sx);
            if (flipX) reverseX(tmpShared.get(),sy,sx);
            if (flipY) reverseY(tmpShared.get(),sy,sx);
        }
    }
    
    return Point(images.dimSize(1),images.dimSize(2));
    
}

