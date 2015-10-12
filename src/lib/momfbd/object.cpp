#include "redux/momfbd/object.hpp"

#include "redux/momfbd/momfbdjob.hpp"
#include "redux/momfbd/channel.hpp"
#include "redux/momfbd/data.hpp"
#include "redux/momfbd/util.hpp"

#include "redux/file/fileana.hpp"
#include "redux/file/filemomfbd.hpp"
#include "redux/image/utils.hpp"
#include "redux/math/functions.hpp"
#include "redux/translators.hpp"
#include "redux/util/datautil.hpp"
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
using namespace redux::math;
using namespace redux::image;
using namespace redux::util;
using namespace redux;
using namespace std;
using boost::algorithm::iequals;

#define lg Logger::mlg
namespace {

    const string thisChannel = "object";
    
    bool checkImageScale (double& F, double& A, double& P) {

        double rad2asec = 180.0 * 3600.0 / redux::PI;
        size_t count = F > 0 ? 1 : 0;
        count += A > 0 ? 1 : 0;
        count += P > 0 ? 1 : 0;
        if (count > 2) {
            LOG_WARN << "Too many parameters specified: replacing telescope focal length (" << F
                     << ") with computed value (" << (P * rad2asec / A) << ")";
            F = P * rad2asec / A;
            return true;
        } else if (count < 2) {
            LOG_ERR << "At least two of the parameters \"TELESCOPE_F\", \"ARCSECPERPIX\" and \"PIXELSIZE\" has to be provided.";
        } else {    // count == 2
            if (F <= 0) {
                F = P * rad2asec / A;
            } else if (A <= 0) {
                A = P * rad2asec / F;
            } else if (P <= 0) {
                P = F * A / rad2asec;
            }
            return true;
        }
        return false;
    }

    
}


Object::Object (MomfbdJob& j, uint16_t id) : ObjectCfg (j), myJob (j), currentMetric(0), reg_gamma(0),
    frequencyCutoff(0),pupilRadiusInPixels(0), ID (id), objMaxMean(0), imgSize(0), nObjectImages(0) {


}


Object::~Object() {

}


void Object::parsePropertyTree( bpt::ptree& tree ) {

    ObjectCfg::parseProperties(tree, myJob);

    uint16_t nCh(0);
    for( auto & property : tree ) {
        if( iequals( property.first, "CHANNEL" ) ) {
            Channel* tmpCh = new Channel( *this, myJob, nCh++ );
            tmpCh->parsePropertyTree( property.second );
            channels.push_back( shared_ptr<Channel>( tmpCh ) );
        }
    }

    //LOG_DEBUG << "Object::parseProperties() done.";

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
    sz += 2*sizeof(uint16_t) + 4*sizeof(double);                   // channels.size() + ID + maxMean
    for( const shared_ptr<Channel>& ch : channels ) {
        sz += ch->size();
    }
    sz += imgSize.size();
    sz += sizeof(nObjectImages);
    return sz;
}


uint64_t Object::pack(char* ptr) const {
    using redux::util::pack;
    uint64_t count = ObjectCfg::pack(ptr);
    count += pack(ptr+count, ID);
    count += pack(ptr+count, currentMetric);
    count += pack(ptr+count, frequencyCutoff);
    count += pack(ptr+count, pupilRadiusInPixels);
    count += pack(ptr+count, objMaxMean);
    count += imgSize.pack(ptr+count);
    count += pack(ptr+count, (uint16_t)channels.size());
    for( const shared_ptr<Channel>& ch : channels ) {
        count += ch->pack(ptr+count);
    }
    count += pack (ptr + count, nObjectImages);
    if (count != size()) {
        LOG_ERR << "(" << hexString (this) << "): Packing failed, there is a size mismatch:  count = " << count << "  sz = " << size();
    }
    return count;
}


uint64_t Object::unpack(const char* ptr, bool swap_endian) {
    using redux::util::unpack;

    uint64_t count = ObjectCfg::unpack(ptr, swap_endian);
    count += unpack(ptr+count, ID, swap_endian);
    count += unpack(ptr+count, currentMetric, swap_endian);
    count += unpack(ptr+count, frequencyCutoff, swap_endian);
    count += unpack(ptr+count, pupilRadiusInPixels, swap_endian);
    count += unpack(ptr+count, objMaxMean, swap_endian);
    count += imgSize.unpack(ptr+count, swap_endian);
    uint16_t tmp;
    count += unpack(ptr+count, tmp, swap_endian);
    channels.resize(tmp);
    for( shared_ptr<Channel>& ch : channels ) {
        ch.reset(new Channel(*this,myJob));
        count += ch->unpack(ptr+count, swap_endian);
    }
    count += unpack (ptr + count, nObjectImages, swap_endian);
    return count;
}


uint32_t Object::nImages(void) const {
    if(nObjectImages) return nObjectImages;
    for (const shared_ptr<Channel>& ch : channels) nObjectImages += ch->nImages();
    return nObjectImages;
}


void Object::initProcessing( const Solver& ws ) {

    if( patchSize && pupilPixels ) {
        P.resize(2*pupilPixels,2*pupilPixels);
        Q.resize(2*pupilPixels,2*pupilPixels);
        ftSum.resize(patchSize,patchSize);          // full-complex for now
        for( auto& ch : channels ) {
            ch->initProcessing(ws);
        }
        initCache();
        //modes.init( myJob, *this );                 // will get modes from globalData
    } else {
        LOG_ERR << "Object patchSize is 0 !!!";
    }
    
}


void Object::initPatch( ObjectData& od ) {
    unique_lock<mutex> lock (mtx);
    reg_gamma = 0;
    ftSum.zero();
}


void Object::getResults(ObjectData& od) {
    
    unique_lock<mutex> lock (mtx);
    
    FourierTransform avgObjFT(patchSize, patchSize, FT_FULLCOMPLEX|FT_REORDER );
    Array<complex_t> tmpC(patchSize, patchSize);
    Array<double> tmpD(patchSize, patchSize);
    avgObjFT.zero();
    tmpD.zero();
    complex_t* aoPtr = avgObjFT.get();
    double* dPtr = tmpD.get();
    double avgNoiseVariance = 0.0;
    for (shared_ptr<Channel>& ch : channels) {
        for (shared_ptr<SubImage>& im : ch->subImages) {
            im->restore(aoPtr, dPtr);
            avgNoiseVariance += sqr(im->stats.noise);
        }
    }
    avgNoiseVariance /= nObjectImages;
    
    avgObjFT.safeDivide(tmpD);
    
    if (!(myJob.runFlags&RF_NO_FILTER)) {
        LOG_TRACE << boost::format("Applying Scharmer filter with frequency-cutoff = %f and noise-variance = %f") % (0.9*frequencyCutoff) % avgNoiseVariance;
        ScharmerFilter(aoPtr, dPtr, patchSize, patchSize, avgNoiseVariance, 0.90 * frequencyCutoff);
    }
    
    avgObjFT.directInverse(tmpC.get());
    od.img.resize(patchSize, patchSize);
    od.img.assign(tmpC);
    
    if( fittedPlane.sameSize(od.img) ) {
        LOG_DETAIL << "Re-adding fitted plane to result.";
        od.img += fittedPlane;
    } else if( !fittedPlane.empty() ) {
        LOG_WARN << "Size mismatch when re-adding fitted plane.";
    }

    

    // PSF
    if( saveMask & (SF_SAVE_PSF|SF_SAVE_PSF_AVG) ) {
        uint16_t nPSF = (saveMask&SF_SAVE_PSF_AVG)? 1 : nObjectImages;
        od.psf.resize(nPSF, patchSize, patchSize);
        od.psf.zero();
        if( nPSF > 1 ) {
            Array<float> view(od.psf,0,0,0,patchSize-1,0,patchSize-1);
            for( shared_ptr<Channel>& ch: channels) {
                for ( shared_ptr<SubImage>& si: ch->subImages) {
                    view.assign(si->getPSF<complex_t>()); //si->getPSF());
                    view.shift(0,1);
                }
            }
        } else if( nPSF == 1 ) {
            for( shared_ptr<Channel>& ch: channels) {
                for ( shared_ptr<SubImage>& si: ch->subImages) {
                    si->addPSF(od.psf);
                }
            }
            od.psf *= (1.0/nObjectImages);
        }
    }

    // Convolved objects
    if( saveMask & SF_SAVE_COBJ ) {
        if( nObjectImages ) {
            od.cobj.resize(nObjectImages, patchSize, patchSize);
            od.cobj.zero();
            Array<float> view(od.cobj,0,0,0,patchSize-1,0,patchSize-1);
            for( shared_ptr<Channel>& ch: channels) {
                for ( shared_ptr<SubImage>& si: ch->subImages) {
                    view.assign(si->convolveImage(od.img));
                    view.shift(0,1);
                }
            }
        } else {
            od.cobj.clear();
        }
    }

    // Residuals
    if( saveMask & SF_SAVE_RESIDUAL ) {
        if( nObjectImages  ) {
            od.res.resize(nObjectImages, patchSize, patchSize);
            od.res.zero();
            Array<float> view(od.res,0,0,0,patchSize-1,0,patchSize-1);
            if( od.cobj.sameSizes(od.res) ) {
                Array<float> cview(od.cobj,0,0,0,patchSize-1,0,patchSize-1);
                for( shared_ptr<Channel>& ch: channels) {
                    for ( shared_ptr<SubImage>& si: ch->subImages) {
                        view.assign(si->convolvedResidual(cview));
                        view.shift(0,1);
                        cview.shift(0,1);
                    }
                }
            } else {
                for( shared_ptr<Channel>& ch: channels) {
                    for ( shared_ptr<SubImage>& si: ch->subImages) {
                        view.assign(si->residual(od.img));
                        view.shift(0,1);
                    }
                }
            }
        } else {
            od.res.clear();
        }
    }
    
    // Mode coefficients
    if( saveMask & SF_SAVE_ALPHA) {
        if( nObjectImages  ) {
            size_t nModes = myJob.modeNumbers.size();
            od.alpha.resize(nObjectImages, myJob.modeNumbers.size());
            od.alpha.zero();
            float* alphaPtr = od.alpha.get();
            for( shared_ptr<Channel>& ch: channels ) {
                for ( shared_ptr<SubImage>& si: ch->subImages ) {
                    si->getAlphas(alphaPtr);
                    alphaPtr += nModes;
                }
            }
        } else {
            od.alpha.clear();
        }

    }

    // Diversity
    if( saveMask & SF_SAVE_DIVERSITY) {
        int nCh = channels.size();
        if( nCh  ) {
            od.div.resize(nCh, pupilPixels, pupilPixels);
            Array<float> view(od.div,0,0,0,pupilPixels-1,0,pupilPixels-1);
            for( shared_ptr<Channel>& ch: channels) {
                view = ch->phi_fixed;
                view.shift(0,1);
            }
        } else {
            od.div.clear();
        }

    }

}


void Object::initPQ (void) {
    P.zero();
    Q = reg_gamma;
}


void Object::addRegGamma( double rg ) {
    unique_lock<mutex> lock( mtx );
    reg_gamma += 0.10*rg/nObjectImages;
}


void Object::addToFT( const redux::image::FourierTransform& ft ) {
    unique_lock<mutex> lock( mtx );
    transform(ftSum.get(), ftSum.get()+ftSum.nElements(), ft.get(), ftSum.get(),
              [](const double& a, const complex_t& b) { return a+norm(b); }
             );

}


void Object::addDiffToFT( const Array<complex_t>& ft, const Array<complex_t>& oldft ) {
    unique_lock<mutex> lock( mtx );
    const complex_t* oftPtr = oldft.get();
    transform(ftSum.get(), ftSum.get()+ftSum.nElements(), ft.get(), ftSum.get(),
              [&oftPtr](const double& a, const complex_t& b) { return a+norm(b)-norm(*oftPtr++); }
             );
}


void Object::addDiffToPQ(const redux::image::FourierTransform& ft, const Array<complex_t>& otf, const Array<complex_t>& oldotf) {

    unique_lock<mutex> lock (mtx);
    double *qPtr = Q.get();
    complex_t *pPtr = P.get();
    const complex_t *ftPtr = ft.get();
    const complex_t *otfPtr = otf.get();
    const complex_t *ootfPtr = oldotf.get();

    for( auto ind: pupil.otfSupport ) {
        qPtr[ind] += norm(otfPtr[ind]) - norm(ootfPtr[ind]);
        pPtr[ind] += conj(ftPtr[ind]) * (otfPtr[ind] - ootfPtr[ind]);
    }
}


void Object::addToPQ(const redux::image::FourierTransform& ft, const Array<complex_t>& otf) {

    unique_lock<mutex> lock (mtx);
    double *qPtr = Q.get();
    complex_t *pPtr = P.get();
    const complex_t *ftPtr = ft.get();
    const complex_t *otfPtr = otf.get();

    for( auto ind: pupil.otfSupport ) {
        qPtr[ind] += norm(otfPtr[ind]);
        pPtr[ind] += conj(ftPtr[ind]) * otfPtr[ind];
    }
    
}


void Object::addAllPQ(void) {
    for( shared_ptr<Channel>& ch : channels ) {
        for( shared_ptr<SubImage>& im : ch->subImages ) {
            unique_lock<mutex> lock( mtx );
            im->addPQ(P.get(),Q.get());
        }
    }
}


void Object::fitAvgPlane(ObjectData& od) {
    
    if( (myJob.runFlags&RF_FIT_PLANE) && od.channels.size() && od.channels[0].images.nDimensions() == 3 ) {
        
        size_t count(0);
        size_t ySize = od.channels[0].images.dimSize(1);
        size_t xSize = od.channels[0].images.dimSize(2);
        
        fittedPlane.resize(ySize,xSize);
        fittedPlane.zero();
        
        for( ChannelData& cd : od.channels ) {
            size_t nImages = cd.images.dimSize(0);
            vector<int64_t> first = cd.images.first();
            vector<int64_t> last = cd.images.last();
            last[0] = first[0];                         // only select first image
            Array<float> view(cd.images, first, last);
            for( uint i=0; i<nImages; ++i ) {
                if( ! view.sameSize(fittedPlane) ) {
                    LOG_ERR << "Size mismatch when fitting average plane for object #" << ID;
                    fittedPlane.clear();
                    return;
                }
                fittedPlane += view;
                view.shift(0,1);
                count++;
            }
        }
        
        if(count) {
            fittedPlane /= count;
            fittedPlane = fitPlane(fittedPlane, true);          // fit plane to the average image, and subtract average
        } else {
            fittedPlane.clear();
            return;
        }
        
        LOG_DETAIL << "Subtracting average plane before processing.";
        
        for( ChannelData& cd : od.channels ) {
            size_t nImages = cd.images.dimSize(0);
            vector<int64_t> first = cd.images.first();
            vector<int64_t> last = cd.images.last();
            last[0] = first[0];                         // only select first image
            Array<float> view(cd.images, first, last);
            for( uint i=0; i<nImages; ++i ) {
                view -= fittedPlane;                    // subract fitted plane from all images
                view.shift(0, 1);                       // shift view to next image in stack
            }
        }
        
        fittedPlane.setLimits( myJob.maxLocalShift, myJob.maxLocalShift+myJob.patchSize-1, myJob.maxLocalShift, myJob.maxLocalShift+myJob.patchSize-1);
        fittedPlane.trim();             // we fit/subtract the whole cutout area, but only re-add it for the patch, so store it in that size.
        transpose(fittedPlane.get(),fittedPlane.dimSize(0),fittedPlane.dimSize(1));                 // to match the transposed subimage.
    }


}



void Object::calcMetric (void) {
    
    const double* ftsPtr = ftSum.get();
    const complex_t* pPtr = P.get();
    const double* qPtr = Q.get();
    
    unique_lock<mutex> lock (mtx);
    size_t N = 4*pupilPixels*pupilPixels;
    for (size_t ind=0; ind<N; ++ind) {
    //for (auto & ind : otfIndices) {
        currentMetric += (ftsPtr[ind] - norm (pPtr[ind]) / qPtr[ind]);
    }

    currentMetric *= (weight / N);

}


bool Object::checkCfg (void) {

    if ( (saveMask & SF_SAVE_PSF) && (saveMask & SF_SAVE_PSF_AVG)) {
        LOG_WARN << "Both GET_PSF and GET_PSF_AVG mode specified.";
    }
    if (channels.empty()) {
        LOG_CRITICAL << "Each object must have at least 1 channel specified.";
    }

    for (shared_ptr<Channel>& ch : channels) {
        if (!ch->checkCfg()) return false;
    }

    if (!checkImageScale (telescopeF, arcSecsPerPixel, pixelSize)) {
        return false;
    }

    if (outputFileName.empty()) {   // TODO: clean this up
        string tpl = channels[0]->imageTemplate;
        size_t p = tpl.find_first_of ('%');
        if (p != string::npos) {
            string tmpString = boost::str (boost::format (tpl) % 1);
            auto it = tmpString.begin();
            auto it2 = tpl.begin();
            p = 0;
            size_t i = std::min (tmpString.length(), tpl.length());
            while (p < i && tmpString[p] == tpl[p]) p++;
            it = tmpString.end();
            it2 = tpl.end();
            size_t ii = tmpString.length() - 1;
            i = tpl.length() - 1;
            while (ii && i && tmpString[ii] == tpl[i]) {
                ii--;
                i--;
            }
            tmpString.replace (p, ii - p + 1, "%d..%d");
            if (count (tmpString.begin(), tmpString.end(), '%') == 2) {
                outputFileName = boost::str (boost::format (tmpString) % *channels[0]->imageNumbers.begin() % *channels[0]->imageNumbers.rbegin());
            } else {
                LOG_CRITICAL << boost::format ("failed to generate output filename from \"%s\"  (->\"%s\").") % tpl % tmpString;
                return false;
            }
        } else LOG_CRITICAL << boost::format ("first filename template \"%s\" does not contain valid format specifier.") % tpl;
    }
    
    /*if( !modeFile.empty() && !modeNumbers.empty() ) {
        LOG_WARN << "A modefile was specified together with mode-numbers. BE AWARE that these numbers will be interpreted as indices in the supplied file and NOT Zernike/KL indices!!";
        return false;
    }*/
    

    return true;
}


bool Object::checkData (void) {

    bfs::path tmpOF(outputFileName + ".ext");
    bfs::path tmpPath = tmpOF.parent_path();
    if( !bfs::exists (tmpPath) ) {
        if( !bfs::create_directories(tmpPath) ) {
            LOG_CRITICAL << boost::format ("failed to create directory for output: %s") % tmpPath;
            return false;
        } else LOG_TRACE << boost::format ("create output directory %s") % tmpPath;
    }
    try {
        bfs::path slask(tmpPath);
        slask += "_test_writability_";
        bfs::create_directory(slask);
        bfs::remove_all(slask);
    } catch( ... ) {
        LOG_CRITICAL << boost::format ("output directory %s not writable: %s") % tmpPath;
        return false;
    }
    for (int i = 1; i & FT_MASK; i <<= 1) {
        if (i & myJob.outputFileType) {  // this filetype is specified.
            tmpOF.replace_extension (FileTypeExtensions.at ( (FileType) i));
            if( bfs::exists(tmpOF) && !(myJob.runFlags & RF_FORCE_WRITE) ) {
                LOG_CRITICAL << boost::format ("output file %s already exists! Use -f (or OVERWRITE) to replace file.") % tmpOF;
                return false;
            } else {
                LOG << "Output filename: " << tmpOF;
            }
        }
    }

    for (shared_ptr<Channel>& ch : channels) {
        if (!ch->checkData()) return false;
    }
    
    if (!pupilFile.empty()) {
        if (! bfs::exists (bfs::path (pupilFile))) {
            bfs::path fn = bfs::path (imageDataDir) / bfs::path (pupilFile);
            if (! bfs::exists (fn)) {
                //logAndThrow("Pupil-file " + pupilFile + " not found!");
                LOG_CRITICAL << boost::format ("Pupil-file %s not found!") % pupilFile;
                return false;
            } else pupilFile = fn.c_str();
        }
    }

    if (!modeFile.empty()) {
        if (! bfs::exists (bfs::path (modeFile))) {
            bfs::path fn = bfs::path (imageDataDir) / bfs::path (modeFile);
            if (! bfs::exists (fn)) {
                //logAndThrow("Mode-file " + modeFile + " not found!");
                LOG_CRITICAL << boost::format ("Mode-file %s not found!") % modeFile;
                return false;
            } else modeFile = fn.c_str();
        }
    }


    return true;
}


void Object::initCache (void) {
    
    for (shared_ptr<Channel>& ch : channels) {
        ch->initCache();
    }
    
    Pupil::calculatePupilSize (frequencyCutoff, pupilRadiusInPixels, pupilPixels, wavelength, patchSize, myJob.telescopeD, arcSecsPerPixel);
    
    myJob.patchSize = patchSize;     // TODO: fulhack until per-channel sizes is implemented
    myJob.pupilPixels = pupilPixels;

    if ( bfs::exists( bfs::path( pupilFile ) ) ) {
        PupilInfo info( pupilFile, pupilPixels );
        Pupil& ret = myJob.globalData->get(info);
        unique_lock<mutex> lock(ret.mtx);
        if( ret.empty() ) {    // this set was inserted, so it is not loaded yet.
            if( ret.load( pupilFile, pupilPixels ) ) {
                LOG_DEBUG << "Loaded Pupil-file " << pupilFile;
                pupil = ret;
            } else LOG_ERR << "Failed to load Pupil-file " << pupilFile;
        } else {
            if( ret.nPixels && ret.nPixels == pupilPixels ) {    // matching modes
                pupil = ret;
            } else {
                LOG_ERR << "The Cache returned a non-matching Pupil. This might happen if a loaded Pupil was rescaled (which is not implemented yet).";
            }
        }
    }
    
    if( pupil.empty() ) {
        PupilInfo info(pupilPixels, pupilRadiusInPixels);
        Pupil& ret = myJob.globalData->get(info);
        unique_lock<mutex> lock(ret.mtx);
        if( ret.empty() ) {    // this set was inserted, so it is not generated yet.
            ret.generate( pupilPixels, pupilRadiusInPixels );
            if( ret.nDimensions() != 2 || ret.dimSize(0) != pupilPixels || ret.dimSize(1) != pupilPixels ) {    // mismatch
                LOG_ERR << "Generated Pupil does not match. This should NOT happen!!";
            } else {
                LOG_DEBUG << "Generated pupil (" << pupilPixels << "x" << pupilPixels << "  radius=" << pupilRadiusInPixels << ")";
                pupil = ret; 
            }
        } else {
            if( ret.nPixels && ret.nPixels == pupilPixels ) {    // matching modes
                pupil = ret;
            } else {
                LOG_ERR << "The Cache returned a non-matching Pupil. This should NOT happen!!";
            }
        }
    }

    if ( bfs::exists( bfs::path( modeFile ) ) ) {
        ModeInfo info( modeFile, pupilPixels );
        ModeSet& ret = myJob.globalData->get(info);
        unique_lock<mutex> lock(ret.mtx);
        if( ret.empty() ) {    // this set was inserted, so it is not loaded yet.
            if( ret.load( modeFile, pupilPixels ) ) {
                LOG_DEBUG << "Loaded Mode-file " << modeFile;
                ret.normalize( pupil );
                modes = ret;
              } else LOG_ERR << "Failed to load Mode-file " << modeFile;
        } else {
            if( ret.info.nPupilPixels && ret.info.nPupilPixels == pupilPixels ) {    // matching modes
                modes = ret;
            } else {
                LOG_ERR << "The Cache returned a non-matching ModeSet. This might happen if a loaded ModeSet was rescaled (which is not implemented yet).";
            }
        }
    }
    
    if( modes.empty() ) {
        ModeInfo info(myJob.klMinMode, myJob.klMaxMode, 0, pupilPixels, pupilRadiusInPixels, rotationAngle, myJob.klCutoff);
        if (myJob.modeBasis == ZERNIKE) {     // force use of Zernike modes for all tilts
            info.firstMode = info.lastMode = 0;
        }
        ModeSet& ret = myJob.globalData->get(info);
        unique_lock<mutex> lock(ret.mtx);
        if( ret.empty() ) {    // this set was inserted, so it is not generated yet.
            if(myJob.modeBasis == ZERNIKE) {
                ret.generate( pupilPixels, pupilRadiusInPixels, rotationAngle, myJob.modeNumbers );
            } else {
                ret.generate( pupilPixels, pupilRadiusInPixels, rotationAngle, myJob.klMinMode, myJob.klMaxMode, myJob.modeNumbers, myJob.klCutoff );
            }
            if( ret.nDimensions() != 3 || ret.dimSize(1) != pupilPixels || ret.dimSize(2) != pupilPixels ) {    // mismatch
                LOG_ERR << "Generated ModeSet does not match. This should NOT happen!!";
            } else {
                LOG_DEBUG << "Generated Modeset with " << ret.dimSize(0) << " modes. (" << pupilPixels << "x" << pupilPixels << "  radius=" << pupilRadiusInPixels << ")";
                ret.normalize( pupil );
                modes = ret; 
            }
        } else {
            if( ret.info.nPupilPixels && ret.info.nPupilPixels == pupilPixels ) {    // matching modes
                modes = ret;
            } else {
                LOG_ERR << "The Cache returned a non-matching ModeSet. This should NOT happen!!";
            }
        }
    }
    
    
    pixelsToAlpha =  alphaToPixels = 0;
    if( modes.yTiltIndex >= 0 ) {
        double delta = modes(modes.yTiltIndex,pupilPixels/2+1,pupilPixels/2) - modes(modes.yTiltIndex,pupilPixels/2,pupilPixels/2);
        pixelsToAlpha = util::pix2cf(arcSecsPerPixel,myJob.telescopeD)/(0.5*frequencyCutoff*delta);
    } else if ( modes.xTiltIndex >= 0 ) {
        double delta = modes(modes.xTiltIndex,pupilPixels/2,pupilPixels/2+1) - modes(modes.xTiltIndex,pupilPixels/2,pupilPixels/2);
        pixelsToAlpha = util::pix2cf(arcSecsPerPixel,myJob.telescopeD)/(0.5*frequencyCutoff*delta);
    }
    
    if( fabs(pixelsToAlpha) > 0 ) {
        alphaToPixels = 1.0/pixelsToAlpha;
    }

    defocusToAlpha = util::def2cf(myJob.telescopeD/2.0);
    alphaToDefocus = 1.0/defocusToAlpha;

}


void Object::loadData( boost::asio::io_service& service, Array<PatchData::Ptr>& patches ) {
    
    nObjectImages = nImages();
    startT = bpx::pos_infin;
    endT = bpx::neg_infin;
    
    for (shared_ptr<Channel>& ch : channels) {
        ch->loadCalib(service);
    }
    runThreadsAndWait(service, myJob.info.maxThreads);
    
    objMaxMean = std::numeric_limits<double>::lowest();
    for (shared_ptr<Channel>& ch : channels) {
        ch->loadData(service, patches);
        runThreadsAndWait(service, myJob.info.maxThreads);
        objMaxMean = std::max(objMaxMean,ch->getMaxMean());
        if(startT.is_special()) startT = ch->startT;
        else startT = std::min(startT,ch->startT);
        if(endT.is_special()) endT = ch->endT;
        else endT = std::max(endT,ch->endT);
    }

    
}


void Object::writeAna (const redux::util::Array<PatchData::Ptr>& patches) {

    LOG << "BARELY writing output to ANA.   baseName=\"" << outputFileName << "\"";
    
    LOG_WARN << "Writing to ANA still not properly implemented...";

    for (uint y = 0; y < patches.dimSize(0); ++y) {
        for (uint x = 0; x < patches.dimSize(1); ++x) {
            bfs::path fn = bfs::path (outputFileName + "_img_"+to_string(x)+"_"+to_string(y)+".f0");
            Ana::write(fn.string(), patches(y,x)->objects[ID].img);
        }
    }

    if( saveMask & SF_SAVE_ALPHA ) {
        bfs::path fn = bfs::path (outputFileName + ".alpha.f0");
        LOG << "Saving alpha-coefficients to: " << fn;
        Array<float> alpha(patches.dimSize(0), patches.dimSize(1), nObjectImages, myJob.modeNumbers.size());
        for( auto& patch: patches ) {
            Array<float> subalpha(alpha, patch->index.y, patch->index.y, patch->index.x, patch->index.x, 0, nObjectImages-1, 0, myJob.modeNumbers.size()-1);
            patch->objects[ID].alpha.copy(subalpha);
       }
       Ana::write(fn.string(), alpha);
    }
    
}


void Object::writeFits (const redux::util::Array<PatchData::Ptr>& patches) {
    bfs::path fn = bfs::path (outputFileName + ".fits");
    LOG << "NOT writing output to file: " << fn;
    LOG_ERR << "Writing to FITS still not implemented...";
}


void Object::writeMomfbd (const redux::util::Array<PatchData::Ptr>& patchesData) {

    bfs::path fn = bfs::path (outputFileName + "_thi.momfbd");      // TODO: fix storage properly

    LOG << "Writing output to file: " << fn;

    std::shared_ptr<FileMomfbd> info (new FileMomfbd());

    // Extract date/time from the git commit.
    int day, month, year, hour;
    char buffer [15];
    sscanf (reduxCommitTime, "%4d-%2d-%2d %2d", &year, &month, &day, &hour);
    sprintf (buffer, "%4d%02d%02d.%01d", year, month, day, hour);
    info->versionString = buffer;
    info->version = atof (info->versionString.c_str());

    info->dateString = myJob.observationDate;
    if(startT.is_special() && endT.is_special()) {
        info->timeString = "N/A";
    } else if(startT.is_special()) {
        info->timeString = bpx::to_simple_string(endT.time_of_day());
    } else if(endT.is_special()) {
        info->timeString = bpx::to_simple_string(startT.time_of_day());
    } else {
        bpx::time_duration obs_interval = (endT - startT);
        info->timeString = bpx::to_simple_string((startT+obs_interval/2).time_of_day());
    }
    
    
    int32_t nChannels = info->nChannels = channels.size();
    info->clipStartX = sharedArray<int16_t> (nChannels);
    info->clipEndX = sharedArray<int16_t> (nChannels);
    info->clipStartY = sharedArray<int16_t> (nChannels);
    info->clipEndY = sharedArray<int16_t> (nChannels);
    info->fileNames.clear();
    for (int i = 0; i < nChannels; ++i) {
        channels[i]->getFileNames(info->fileNames);
        if(channels[i]->alignClip.empty()) {
            Point16 sz = channels[i]->getImageSize();
            info->clipStartX.get()[i] = info->clipStartY.get()[i] = 1;
            info->clipEndX.get()[i] = sz.x;
            info->clipEndY.get()[i] = sz.y;
        } else {
            info->clipStartX.get()[i] = channels[i]->alignClip[0];
            info->clipEndX.get()[i] = channels[i]->alignClip[1];
            info->clipStartY.get()[i] = channels[i]->alignClip[2];
            info->clipEndY.get()[i] = channels[i]->alignClip[3];
        }
    }
    info->nPH = pupilPixels;

    uint8_t writeMask = MOMFBD_IMG;                                                 // always output image
    int64_t imgSize = patchSize*patchSize*sizeof(float);
    
    if( info->fileNames.size() ) writeMask |= MOMFBD_NAMES;
    if( saveMask & (SF_SAVE_PSF|SF_SAVE_PSF_AVG) ) writeMask |= MOMFBD_PSF;
    if( saveMask & SF_SAVE_MODES && (info->nPH > 0) ) writeMask |= MOMFBD_MODES;
    if( saveMask & SF_SAVE_COBJ ) writeMask |= MOMFBD_OBJ;
    if( saveMask & SF_SAVE_RESIDUAL ) writeMask |= MOMFBD_RES;
    if( saveMask & SF_SAVE_ALPHA ) writeMask |= MOMFBD_ALPHA;
    if( saveMask & SF_SAVE_DIVERSITY ) writeMask |= MOMFBD_DIV;
    
    Array<float> tmpModes;
    if ( writeMask&MOMFBD_MODES ) {     // copy modes from local cache
        //double pupilRadiusInPixels = pupilPixels / 2.0;
        //if (channels.size()) pupilRadiusInPixels = channels[0]->pupilRadiusInPixels;
        tmpModes.resize (myJob.modeNumbers.size() + 1, info->nPH, info->nPH);            // +1 to also fit pupil in the array
        tmpModes.zero();
        Array<float> tmp_slice (tmpModes, 0, 0, 0, info->nPH - 1, 0, info->nPH - 1);     // subarray
        tmp_slice.assign(reinterpret_cast<const Array<double>&>(pupil));
        info->phOffset = 0;
        tmp_slice.wrap(tmpModes, 1, myJob.modeNumbers.size(), 0, info->nPH - 1, 0, info->nPH - 1);
        tmp_slice.assign(reinterpret_cast<const Array<double>&>(modes));
        if (myJob.modeNumbers.size()) {
            //ModeInfo id (myJob.klMinMode, myJob.klMaxMode, 0, pupilPixels, pupilRadiusInPixels, rotationAngle, myJob.klCutoff);
            info->nModes = myJob.modeNumbers.size();
            info->modesOffset = pupilPixels * pupilPixels * sizeof (float);
            /*for (uint16_t & it : myJob.modeNumbers) {   // Note: globalData might also contain modes we don't want to save here, e.g. PhaseDiversity modes.
                if (it > 3 && myJob.modeBasis != ZERNIKE) {       // use Zernike modes for the tilts
                    id.firstMode = myJob.klMinMode;
                    id.lastMode = myJob.klMaxMode;
                } else id.firstMode = id.lastMode = 0;
                tmp_slice.shift (0, 1);     // shift subarray 1 step
                id.modeNumber = it;
                tmp_slice.assign(reinterpret_cast<const Array<double>&>(myJob.globalData->get(id)));
            }*/
        }
    }

    
    info->pix2cf = pixelsToAlpha;
    info->cf2pix = alphaToPixels;
    info->nPatchesY = patchesData.dimSize(0);
    info->nPatchesX = patchesData.dimSize(1);
    info->patches.resize(info->nPatchesX, info->nPatchesY);

    size_t modeSize = tmpModes.nElements()*sizeof (float);
    size_t blockSize = modeSize;

    for (int x = 0; x < info->nPatchesX; ++x) {
        for (int y = 0; y < info->nPatchesY; ++y) {
            info->patches(x,y).region[0] = patchesData(y,x)->roi.first.x+1;         // store as 1-based indices
            info->patches(x,y).region[1] = patchesData(y,x)->roi.last.x+1;
            info->patches(x,y).region[2] = patchesData(y,x)->roi.first.y+1;
            info->patches(x,y).region[3] = patchesData(y,x)->roi.last.y+1;
            info->patches(x,y).nChannels = nChannels;
            info->patches(x,y).nim = sharedArray<int32_t> (nChannels);
            info->patches(x,y).dx = sharedArray<int32_t> (nChannels);
            info->patches(x,y).dy = sharedArray<int32_t> (nChannels);
            for (int i = 0; i < nChannels; ++i) {
                info->patches(x,y).nim.get()[i] = channels[i]->nImages();
                info->patches(x,y).dx.get()[i] = patchesData(y,x)->objects[ID].channels[i].shift.x;
                info->patches(x,y).dy.get()[i] = patchesData(y,x)->objects[ID].channels[i].shift.y;
            }
            blockSize += imgSize;
            if ( writeMask&MOMFBD_PSF ) {
                if(patchesData(y,x)->objects[ID].psf.nDimensions()>1) {
                    info->patches(x,y).npsf = patchesData(y,x)->objects[ID].psf.dimSize(0);
                    blockSize += info->patches(x,y).npsf*imgSize;
                }
            }
            if ( writeMask&MOMFBD_OBJ ) {
                if(patchesData(y,x)->objects[ID].cobj.nDimensions()>1) {
                    info->patches(x,y).nobj = patchesData(y,x)->objects[ID].cobj.dimSize(0);
                    blockSize += info->patches(x,y).nobj*imgSize;
                }
            }
            if ( writeMask&MOMFBD_RES ) {
                if(patchesData(y,x)->objects[ID].res.nDimensions()>1) {
                    info->patches(x,y).nres = patchesData(y,x)->objects[ID].res.dimSize(0);
                    blockSize += info->patches(x,y).nres*imgSize;
                }
            }
            if ( writeMask&MOMFBD_ALPHA ) {
                if(patchesData(y,x)->objects[ID].alpha.nDimensions()==2) {
                    info->patches(x,y).nalpha = patchesData(y,x)->objects[ID].alpha.dimSize(0);
                    info->patches(x,y).nm = patchesData(y,x)->objects[ID].alpha.dimSize(1);
                    blockSize += info->patches(x,y).nalpha*info->patches(x,y).nm*sizeof(float);
                }
            }
            if ( writeMask&MOMFBD_DIV ) {
                if(patchesData(y,x)->objects[ID].div.nDimensions()>1) {
                    info->patches(x,y).ndiv = patchesData(y,x)->objects[ID].div.dimSize(0);
                    info->patches(x,y).nphx = info->nPH;
                    info->patches(x,y).nphy = info->nPH;
                    blockSize += info->patches(x,y).ndiv*info->patches(x,y).nphx*info->patches(x,y).nphy*sizeof(float);
                }
            }
        }   // y-loop
    }   // x-loop


    auto tmp = sharedArray<char> (blockSize);
    memcpy(tmp.get(), tmpModes.get(), modeSize);
    char* tmpPtr = tmp.get();
    int64_t offset = modeSize;
    for (int x = 0; x < info->nPatchesX; ++x) {
        for (int y = 0; y < info->nPatchesY; ++y) {
            memcpy(tmpPtr+offset, patchesData(y,x)->objects[ID].img.get(), imgSize);
            info->patches(x,y).imgPos = offset;
            offset += imgSize;
            memcpy(tmpPtr+offset, patchesData(y,x)->objects[ID].psf.get(), info->patches(x,y).npsf*imgSize);
            info->patches(x,y).psfPos = offset;
            offset += info->patches(x,y).npsf*imgSize;
            memcpy(tmpPtr+offset, patchesData(y,x)->objects[ID].cobj.get(), info->patches(x,y).nobj*imgSize);
            info->patches(x,y).objPos = offset;
            offset += info->patches(x,y).nobj*imgSize;
            memcpy(tmpPtr+offset, patchesData(y,x)->objects[ID].res.get(), info->patches(x,y).nres*imgSize);
            info->patches(x,y).resPos = offset;
            offset += info->patches(x,y).nres*imgSize;
            size_t alphaSize = info->patches(x,y).nalpha*info->patches(x,y).nm*sizeof(float);
            memcpy(tmpPtr+offset, patchesData(y,x)->objects[ID].alpha.get(), alphaSize);
            info->patches(x,y).alphaPos = offset;
            offset += alphaSize;
            size_t divSize = info->patches(x,y).ndiv*info->patches(x,y).nphx*info->patches(x,y).nphy*sizeof(float);
            memcpy(tmpPtr+offset, patchesData(y,x)->objects[ID].div.get(), divSize);
            info->patches(x,y).diversityPos = offset;
            offset += divSize;
        }
    }

    info->write (fn.string(), reinterpret_cast<char*> (tmp.get()), writeMask);

}

void Object::writeResults (const redux::util::Array<PatchData::Ptr>& patches) {
    if (myJob.outputFileType & FT_ANA) writeAna (patches);
    if (myJob.outputFileType & FT_FITS) writeFits (patches);
    if (myJob.outputFileType & FT_MOMFBD) writeMomfbd (patches);
}

void Object::storePatches (WorkInProgress& wip, boost::asio::io_service& service, uint8_t nThreads) {

    bfs::path fn = bfs::path (outputFileName);
    fn.replace_extension ("momfbd");
    std::shared_ptr<FileMomfbd> info (new FileMomfbd (fn.string()));

    LOG_DEBUG << "storePatches()";

    for (auto & part : wip.parts) {
        PatchData::Ptr patch = static_pointer_cast<PatchData> (part);
        LOG_DEBUG << "storePatches() index: (" << patch->index.x << "," << patch->index.y << ")  offset = "
                  << info->patches (patch->index.x , patch->index.y).offset;
        patch->step = MomfbdJob::JSTEP_COMPLETED;
    }

}


Point16 Object::getImageSize (void) {
    if( imgSize == 0 ) {
        for (shared_ptr<Channel>& ch : channels) {
            Point16 tmp = ch->getImageSize();
            if (imgSize == 0) {
                imgSize = tmp;
            } else if (tmp != imgSize) {
                throw std::logic_error ("The images have different sizes for the different channels, please verify the ALIGN_CLIP values.");
            }
        }
    }
    return imgSize;
}


void Object::dump (std::string tag) {
    tag += "_o"+to_string(ID);
    Ana::write (tag + "_ftsum.f0", ftSum);
    Ana::write (tag + "_q.f0", Q);
    Ana::write (tag + "_p.f0", P);
    Ana::write (tag + "_fittedplane.f0", fittedPlane);
    Ana::write (tag + "_pupil.f0", pupil);
    Ana::write (tag + "_modes.f0", modes);
    for (shared_ptr<Channel>& ch : channels) {
        ch->dump(tag);
    }

}
