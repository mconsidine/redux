#include "redux/momfbd/object.hpp"

#include "redux/momfbd/momfbdjob.hpp"
#include "redux/momfbd/channel.hpp"
#include "redux/momfbd/data.hpp"
#include "redux/momfbd/util.hpp"

#include "redux/file/filemomfbd.hpp"
#include "redux/translators.hpp"
#include "redux/util/stringutil.hpp"
#include "redux/constants.hpp"
#include "redux/logger.hpp"
#include "redux/revision.hpp"

#include <cstdio>
#include <limits>
#include <string>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/info_parser.hpp>

namespace bfs = boost::filesystem;
using namespace redux::momfbd;
using namespace redux::file;
using namespace redux::util;
using namespace redux;
using namespace std;
using boost::algorithm::iequals;

#define lg Logger::mlg
namespace {

    const string thisChannel = "momfbdobj";

}


Object::Object( MomfbdJob& j ) : ObjectCfg(j), myJob(j) {
    
    
}


Object::~Object() {
    
}


void Object::parsePropertyTree( bpt::ptree& tree ) {

    ObjectCfg::parseProperties(tree, myJob);

    for( auto & it : tree ) {
        if( iequals( it.first, "CHANNEL" ) ) {
            Channel* tmpCh = new Channel( *this, myJob );
            tmpCh->parsePropertyTree( it.second );
            channels.push_back( shared_ptr<Channel>( tmpCh ) );
        }
    }

    LOG_DEBUG << "Object::parseProperties() done.";

}


bpt::ptree Object::getPropertyTree( bpt::ptree& tree ) {

    bpt::ptree node;
    
    for( shared_ptr<Channel>& ch : channels ) {
        ch->getPropertyTree( node );
    }

    ObjectCfg::getProperties(node,myJob);
    
    tree.push_back( bpt::ptree::value_type( "object", node ) );
    
    return node;
    
}


size_t Object::size(void) const {
    size_t sz = ObjectCfg::size();
    sz += sizeof(uint16_t);                   // channels.size()
    for( const shared_ptr<Channel>& ch : channels ) {
        sz += ch->size();
    }
    return sz;
}


uint64_t Object::pack(char* ptr) const {
    using redux::util::pack;
    uint64_t count = ObjectCfg::pack(ptr);
    count += pack(ptr+count, (uint16_t)channels.size());
    for( const shared_ptr<Channel>& ch : channels ) {
        count += ch->pack(ptr+count);
    }
    if(count != size()) cout << "Obj " << hexString(this) << " has a size mismatch: " << count << "  sz = " << size() << "  diff = " << (size()-count) <<endl;
    return count;
}


uint64_t Object::unpack(const char* ptr, bool swap_endian) {
    using redux::util::unpack;

    uint64_t count = ObjectCfg::unpack(ptr, swap_endian);
    uint16_t tmp;
    count += unpack(ptr+count, tmp, swap_endian);
    channels.resize(tmp);
    for( shared_ptr<Channel>& ch : channels ) {
        ch.reset(new Channel(*this,myJob));
        count += ch->unpack(ptr+count, swap_endian);
    }
   return count;
}


size_t Object::nImages(size_t offset) {
    size_t n(0);
    for( shared_ptr<Channel>& ch : channels ) n += ch->nImages(offset+n);
    return n;
}


void Object::collectImages(redux::util::Array<float>& stack) const {
    for( const shared_ptr<Channel>& ch : channels ) ch->collectImages(stack);
}


void Object::calcPatchPositions(const std::vector<uint16_t>& y, const std::vector<uint16_t>& x) {
    for( shared_ptr<Channel>& ch : channels ) ch->calcPatchPositions(y,x);
}


void Object::initWorkSpace( WorkSpace& ws ) {
    for( shared_ptr<Channel>& ch : channels ) ch->initWorkSpace(ws);
}


bool Object::checkCfg(void) {
    
    if( ( saveMask & SF_SAVE_PSF ) && ( saveMask & SF_SAVE_PSF_AVG ) ) {
        LOG_WARN << "Both GET_PSF and GET_PSF_AVG mode specified.";
    }
    if( channels.empty() ) {
        LOG_CRITICAL << "Each object must have at least 1 channel specified.";
    }
    
    for( shared_ptr<Channel>& ch: channels ) {
        if( !ch->checkCfg() ) return false;
    }
    
    if( outputFileName.empty() ) {  // TODO: clean this up
        string tpl = channels[0]->imageTemplate;
        size_t p = tpl.find_first_of( '%' );
        if( p != string::npos ) {
            string tmpString = boost::str( boost::format( tpl ) % 1 );
            auto it = tmpString.begin();
            auto it2 = tpl.begin();
            p = 0;
            size_t i = std::min( tmpString.length(), tpl.length() );
            while( p < i && tmpString[p] == tpl[p] ) p++;
            it = tmpString.end();
            it2 = tpl.end();
            size_t ii = tmpString.length() - 1;
            i = tpl.length() - 1;
            while( ii && i && tmpString[ii] == tpl[i] ) {
                ii--;
                i--;
            }
            tmpString.replace( p, ii - p + 1, "%d..%d" );
            if( count( tmpString.begin(), tmpString.end(), '%' ) == 2 ) {
                outputFileName = boost::str( boost::format( tmpString ) % *channels[0]->imageNumbers.begin() % *channels[0]->imageNumbers.rbegin() );
            } else {
                LOG_CRITICAL << boost::format( "failed to generate output file from \"%s\"  (->\"%s\")." ) % tpl % tmpString;
                return false;
            }
        }
        else LOG_CRITICAL << boost::format( "first filename template \"%s\" does not contain valid format specifier." ) % tpl;
    }

    bfs::path tmpOF( outputFileName+".ext" );
    for( int i = 1; i&FT_MASK; i <<= 1 ) {
        if( i & myJob.outputFileType ) { // this filetype is specified.
            tmpOF.replace_extension( FileTypeExtensions.at( ( FileType )i ) );
            if( !(myJob.runFlags&RF_FORCE_WRITE) && bfs::exists( tmpOF ) ) {
                LOG_CRITICAL << boost::format( "output file %s already exists! Use -f (or OVERWRITE) to replace file." ) % tmpOF;
                return false;
            } else {
                LOG << "Output filename: " << tmpOF;
            }
        }
    }

    return true;
}


bool Object::checkData(void) {
    
 //   fn = bfs::path( outputFileName );
/*    if( !bfs::exists( fn ) ) {
        LOG_CRITICAL << "Not found !!! \"" << fn.string() << "\"";
        imageNumbers.erase( imageNumbers.begin() + i );
        continue;
    }
  */  

    for( shared_ptr<Channel>& ch : channels ) {
        if( !ch->checkData() ) return false;
    }
    
    return true;
}


void Object::init( void ) {

    for( shared_ptr<Channel>& ch : channels ) {
        ch->init();
    }
    
//   init( KL_cfg* kl_cfg, double lambda, double r_c, int nph_in, int basis, int nm, int *mode_num,
//              int nch, int *ndo, int **dorder, int **dtype, int kl_min_mode, int kl_max_mode, double svd_reg, double angle, double **pupil_in ) 
   
//    modes.init(coeff,lambda,r_c,nph,myJob.basis,myJob.modes);
   
/*    for( int o = 1; o <= nObjects; ++o ) {
        mode[o] = new modes( kl_cfs, cfg->lambda[o], cfg->lim_freq[o] / 2.0, cfg->nph[o], cfg->basis, cfg->nModes, cfg->mode_num, nChannels[o], cfg->nDiversityOrders[o], cfg->dorder[o], cfg->dtype[o], cfg->kl_min_mode, cfg->kl_max_mode, cfg->svd_reg, cfg->angle[o], cfg->pupil[o], io );
//              cfg->pix2cf[o]/=0.5*cfg->lambda[o]*cfg->lim_freq[o]*(mode[o]->mode[0][2][cfg->nph[o]/2+1][cfg->nph[o]/2]-mode[o]->mode[0][2][cfg->nph[o]/2][cfg->nph[o]/2]);
//              cfg->cf2pix[o]*=0.5*cfg->lambda[o]*cfg->lim_freq[o]*(mode[o]->mode[0][2][cfg->nph[o]/2+1][cfg->nph[o]/2]-mode[o]->mode[0][2][cfg->nph[o]/2][cfg->nph[o]/2]);
        cfg->pix2cf[o] /= 0.5 * cfg->lambda[o] * cfg->lim_freq[o] * mode[o]->mode[0][2]->ddx();
        cfg->cf2pix[o] *= 0.5 * cfg->lambda[o] * cfg->lim_freq[o] * mode[o]->mode[0][2]->ddx();
    }
*/


}


void Object::initCache( void ) {

    for( shared_ptr<Channel>& ch : channels ) {
        ch->initCache();
    }
    
//   init( KL_cfg* kl_cfg, double lambda, double r_c, int nph_in, int basis, int nm, int *mode_num,
//              int nch, int *ndo, int **dorder, int **dtype, int kl_min_mode, int kl_max_mode, double svd_reg, double angle, double **pupil_in ) 
   
//    modes.init(coeff,lambda,r_c,nph,myJob.basis,myJob.modes);
   
/*    for( int o = 1; o <= nObjects; ++o ) {
        mode[o] = new modes( kl_cfs, cfg->lambda[o], cfg->lim_freq[o] / 2.0, cfg->nph[o], cfg->basis, cfg->nModes, cfg->mode_num, nChannels[o], cfg->nDiversityOrders[o], cfg->dorder[o], cfg->dtype[o], cfg->kl_min_mode, cfg->kl_max_mode, cfg->svd_reg, cfg->angle[o], cfg->pupil[o], io );
//              cfg->pix2cf[o]/=0.5*cfg->lambda[o]*cfg->lim_freq[o]*(mode[o]->mode[0][2][cfg->nph[o]/2+1][cfg->nph[o]/2]-mode[o]->mode[0][2][cfg->nph[o]/2][cfg->nph[o]/2]);
//              cfg->cf2pix[o]*=0.5*cfg->lambda[o]*cfg->lim_freq[o]*(mode[o]->mode[0][2][cfg->nph[o]/2+1][cfg->nph[o]/2]-mode[o]->mode[0][2][cfg->nph[o]/2][cfg->nph[o]/2]);
        cfg->pix2cf[o] /= 0.5 * cfg->lambda[o] * cfg->lim_freq[o] * mode[o]->mode[0][2]->ddx();
        cfg->cf2pix[o] *= 0.5 * cfg->lambda[o] * cfg->lim_freq[o] * mode[o]->mode[0][2]->ddx();
    }
*/


}


void Object::cleanup( void ) {

}


void Object::loadData( boost::asio::io_service& service ) {
    
    LOG << "Object::loadData()";

    for( shared_ptr<Channel>& ch : channels ) {
        ch->loadData( service );
    }
    
}


void Object::preprocessData(boost::asio::io_service& service ) {
    
    LOG_TRACE << "Object::preprocessData()";
    for( shared_ptr<Channel>& ch : channels ) {
        ch->preprocessData(service);
    }
}


void Object::normalize(boost::asio::io_service& service ) {
    
    LOG_TRACE << "Object::normalize()";
    double maxMean = std::numeric_limits<double>::lowest();
    for( shared_ptr<Channel>& ch : channels ) {
        double mM = ch->getMaxMean();
        if( mM > maxMean ) maxMean = mM;
    }
    for( shared_ptr<Channel>& ch : channels ) {
        ch->normalizeData(service, maxMean);
    }
}


void Object::prepareStorage(void) {

    bfs::path fn = bfs::path( outputFileName + ".momfbd" );     // TODO: fix storage properly
    
    LOG_DEBUG << "Preparing file " << fn << " for temporary, and possibly final, storage.";
    
    std::shared_ptr<FileMomfbd> info ( new FileMomfbd() );

    // Extract date/time from the git commit.
    int day, month, year, hour;
    char buffer [15];
    sscanf(reduxCommitTime, "%4d-%2d-%2d %2d", &year, &month, &day, &hour);
    sprintf (buffer, "%4d%02d%02d.%02d", year, month, day, hour);
    info->versionString = buffer;
    info->version = atof ( info->versionString.c_str() );

    info->dateString = "FIXME";
    info->timeString = "FIXME";
//     if(false) {
//         for( auto& it: channels ) {
//             info->fileNames.push_back ( "FIXME" );
//         }
//         info->dataMask |= MOMFBD_NAMES;
//     }
    info->nFileNames = info->fileNames.size();
    
    int32_t n_img = nImages();
    int32_t nChannels = info->nChannels = channels.size();
    info->clipStartX = sharedArray<int16_t>(nChannels);
    info->clipEndX = sharedArray<int16_t>(nChannels);
    info->clipStartY = sharedArray<int16_t>(nChannels);
    info->clipEndY = sharedArray<int16_t>(nChannels);
    for(int i=0; i<nChannels; ++i ) {
        info->clipStartX.get() [ i ] = channels[i]->alignClip[0];
        info->clipEndX.get() [ i ] = channels[i]->alignClip[1];
        info->clipStartY.get() [ i ] = channels[i]->alignClip[2];
        info->clipEndY.get() [ i ] = channels[i]->alignClip[3];
    }
    
    info->nPH = pupilSize;
    
    Array<float> tmp;
    
    if(saveMask&SF_SAVE_MODES && (info->nPH>0)) {
        tmp.resize(myJob.modeNumbers.size()+1,info->nPH,info->nPH);
        tmp.zero();
        Array<float> tmp_slice(tmp, 0, 0, 0, info->nPH-1, 0, info->nPH-1);
        tmp_slice = 0;//pupil;
        info->phOffset = 0;
        if(myJob.modeNumbers.size()) {
            info->nModes = myJob.modeNumbers.size();
            info->modesOffset = pupilSize*pupilSize*sizeof(float);
            /*for( auto& it: modes ) {
                tmp_slice.shift(0,1);
                tmp_slice = *it.second;
            }*/
        }
    }
    /*
    info->pix2cf = pix2cf;
    info->cf2pix = cf2pix;
    */
    info->nPatchesX = 0;//nPatchesX;
    info->nPatchesY = 0;//nPatchesY;
    info->patches.resize ( info->nPatchesY, info->nPatchesX );
    
    auto dummy = sharedArray<int32_t>(nChannels);
    for ( int x = 0; x < info->nPatchesX; ++x ) {
        for ( int y = 0; y < info->nPatchesY; ++y ) {
            info->patches(x,y).region[0] = info->patches(x,y).region[2] = 1;
            info->patches(x,y).region[1] = info->patches(x,y).region[3] = patchSize;
            info->patches(x,y).nChannels = nChannels;
            info->patches(x,y).nim = sharedArray<int32_t>(nChannels); //dummy;
            info->patches(x,y).dx = sharedArray<int32_t>(nChannels); //dummy;
            info->patches(x,y).dy = sharedArray<int32_t>(nChannels);; //dummy;
            for(int i=0; i<nChannels; ++i) {
                info->patches(x,y).nim.get()[i] = 1000+x*100+y*10+i;
                info->patches(x,y).dx.get()[i] = 2000+x*100+y*10+i;
                info->patches(x,y).dy.get()[i] = 3000+x*100+y*10+i;
            }
            info->patches(x,y).npsf = n_img;
            info->patches(x,y).nobj = n_img;
            info->patches(x,y).nres = n_img;
            info->patches(x,y).nalpha = n_img;
            info->patches(x,y).ndiv = n_img;
            info->patches(x,y).nm = info->nModes;
            info->patches(x,y).nphx = info->nPH;
            info->patches(x,y).nphy = info->nPH;

        }   // y-loop
    }   // x-loop

    
    
    
    uint8_t writeMask = MOMFBD_IMG;                                                 // always output image
    if(saveMask&SF_SAVE_PSF || saveMask&SF_SAVE_PSF_AVG)    writeMask |= MOMFBD_PSF;
    if(saveMask&SF_SAVE_COBJ)    writeMask |= MOMFBD_OBJ;
    if(saveMask&SF_SAVE_RESIDUAL)    writeMask |= MOMFBD_RES;
    if(saveMask&SF_SAVE_ALPHA)    writeMask |= MOMFBD_ALPHA;
    if(saveMask&SF_SAVE_DIVERSITY)    writeMask |= MOMFBD_DIV;
    if(saveMask&SF_SAVE_MODES)    writeMask |= MOMFBD_MODES;
    
    //cout << "prepareStorage: " << bitString(writeMask) << endl;
    info->write ( fn.string(), reinterpret_cast<char*>(tmp.ptr()), writeMask );
    //cout << "prepareStorage done."  << endl;
 
}


void Object::storePatches( WorkInProgress& wip, boost::asio::io_service& service, uint8_t nThreads) {
  
    bfs::path fn = bfs::path( outputFileName );
    fn.replace_extension( "momfbd" );
    std::shared_ptr<FileMomfbd> info ( new FileMomfbd(fn.string()) );

    LOG_DEBUG << "storePatches()";
    
    for( auto& it: wip.parts ) {
        PatchData::Ptr patch = static_pointer_cast<PatchData>(it);
        LOG_DEBUG << "storePatches() index: (" << patch->index.x << "," << patch->index.y << ")  offest = "
        << info->patches( patch->index.x ,patch->index.y).offset;
        patch->step = MomfbdJob::JSTEP_COMPLETED;
    }
  
}


size_t Object::sizeOfPatch(uint32_t npixels) const {
    size_t sz(0);
    for( const shared_ptr<Channel>& ch : channels ) {
        sz += ch->sizeOfPatch(npixels);
    }
    return sz;
}


Point16 Object::clipImages(void) {
    Point16 sizes;
    for( shared_ptr<Channel>& ch : channels ) {
        Point16 tmp = ch->clipImages();
        if(sizes.x == 0) {
            sizes = tmp;
        } else if( tmp != sizes ) {
            throw std::logic_error("The clipped images have different sizes for the different channels, please verify the ALIGN_CLIP values.");
        }
    }
    return sizes;
}

