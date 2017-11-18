#include "redux/image/pupil.hpp"

#include "redux/file/fileana.hpp"
#include "redux/file/fileio.hpp"
#include "redux/image/fouriertransform.hpp"
#include "redux/image/grid.hpp"
#include "redux/image/utils.hpp"
#include "redux/util/stringutil.hpp"
#include "redux/util/arraystats.hpp"

#include <boost/filesystem.hpp>

using namespace redux;
using namespace redux::file;
using namespace redux::image;
using namespace redux::util;

using namespace std;

namespace bfs = boost::filesystem;


PupilInfo::PupilInfo( string filename, uint16_t pixels )
    : nPixels(pixels), pupilRadius(0), filename(filename) {

}


PupilInfo::PupilInfo( uint16_t pixels, double pupilRadius )
    : nPixels(pixels), pupilRadius(pupilRadius), filename("") {

}

      
uint64_t PupilInfo::size( void ) const {
    static uint64_t sz = sizeof(uint16_t) + sizeof(double) + 1;
    sz += filename.length();
    return sz;
}


uint64_t PupilInfo::pack( char* ptr ) const {
    using redux::util::pack;
    uint64_t count = pack(ptr,nPixels);
    count += pack(ptr+count,pupilRadius);
    count += pack(ptr+count,filename);
    return count;
}


uint64_t PupilInfo::unpack( const char* ptr, bool swap_endian ) {
    using redux::util::unpack;
    uint64_t count = unpack(ptr,nPixels,swap_endian);
    count += unpack(ptr+count,pupilRadius,swap_endian);
    count += unpack(ptr+count,filename,swap_endian);
    return count;
}


bool PupilInfo::operator<(const PupilInfo& rhs) const {
    if(filename != rhs.filename) return (filename < rhs.filename);
    if(nPixels != rhs.nPixels) return (nPixels < rhs.nPixels);
    return (pupilRadius < rhs.pupilRadius);
}


PupilInfo::operator string() const {
    string ret = to_string(nPixels)+":"+to_string(pupilRadius)+":";
    return ret;
}


void Pupil::calculatePupilSize (double &frequencyCutoff, double &pupilRadiusInPixels, uint16_t &nPupilPixels, double wavelength, uint32_t nPixels, double telescopeDiameter, double arcSecsPerPixel) {
    static double radians_per_arcsec = M_PI / (180.0 * 3600.0);         // (2.0*PI)/(360.0*3600.0)
    double radians_per_pixel = arcSecsPerPixel * radians_per_arcsec;
    double q_number = wavelength / (radians_per_pixel * telescopeDiameter);
    frequencyCutoff = (double) nPixels / q_number;
    nPupilPixels = nPixels >> 2;
    pupilRadiusInPixels = frequencyCutoff / 2.0;                   // telescope radius in pupil pixels...
    if (nPupilPixels < pupilRadiusInPixels) {            // this should only be needed for oversampled images
        uint16_t goodsizes[] = { 16, 18, 20, 24, 25, 27, 30, 32, 36, 40, 45, 48, 50, 54, 60, 64, 72, 75, 80, 81, 90, 96, 100, 108, 120, 125, 128, 135, 144 };
        for (int i = 0; (nPupilPixels = max (goodsizes[i], nPupilPixels)) < pupilRadiusInPixels; ++i);     // find right size
    }
    nPupilPixels <<= 1;
}


Pupil::Pupil( uint16_t pixels, double pupilRadius )
    : nPixels(pixels), radius(pupilRadius), area(0) {

    generate(pixels,pupilRadius);
    
}

Pupil::Pupil(Pupil&& rhs) : redux::util::Array<double>(std::move(reinterpret_cast<redux::util::Array<double>&>(rhs))),
    nPixels(std::move(rhs.nPixels)), radius(std::move(rhs.radius)),
    area(std::move(rhs.area)), pupilSupport(std::move(rhs.pupilSupport)),
    otfSupport(std::move(rhs.otfSupport)), pupilInOTF(std::move(rhs.pupilInOTF)) {

}


Pupil::Pupil(const Pupil& rhs) : redux::util::Array<double>(reinterpret_cast<const redux::util::Array<double>&>(rhs)),
    nPixels(rhs.nPixels), radius(rhs.radius), area(rhs.area),
    pupilSupport(rhs.pupilSupport), otfSupport(rhs.otfSupport), pupilInOTF(rhs.pupilInOTF)  {
    
}


uint64_t Pupil::size( void ) const {
    uint64_t sz = Array<double>::size();
    sz += sizeof(nPixels) + sizeof(radius) + sizeof(area);
    sz += pupilSupport.size()*sizeof(size_t) + sizeof(uint64_t);
    sz += otfSupport.size()*sizeof(size_t) + sizeof(uint64_t);
    sz += pupilInOTF.size()*2*sizeof(size_t) + sizeof(uint64_t);
    return sz;
}


uint64_t Pupil::pack( char* data ) const {
    using redux::util::pack;
    uint64_t count = Array<double>::pack(data);
    count += pack(data+count,nPixels);
    count += pack(data+count,radius);
    count += pack(data+count,area);
    count += pack(data+count,pupilSupport);
    count += pack(data+count,otfSupport);
    count += pack(data+count,(uint64_t)pupilInOTF.size());
    for( const auto& index: pupilInOTF ) {
        count += pack(data+count,index.first);
        count += pack(data+count,index.second);
    }
    return count;
}


uint64_t Pupil::unpack( const char* data, bool swap_endian ) {
    using redux::util::unpack;
    uint64_t count = Array<double>::unpack(data,swap_endian);
    count += unpack(data+count,nPixels,swap_endian);
    count += unpack(data+count,radius,swap_endian);
    count += unpack(data+count,area,swap_endian);
    count += unpack(data+count,pupilSupport,swap_endian);
    count += unpack(data+count,otfSupport,swap_endian);
    uint64_t tmp;
    count += unpack(data+count,tmp,swap_endian);
    pupilInOTF.resize(tmp);
    for( auto& index: pupilInOTF ) {
        count += unpack(data+count,index.first,swap_endian);
        count += unpack(data+count,index.second,swap_endian);
    }
    return count;
}


bool Pupil::load( const string& filename, uint16_t pixels ) {
    
    if ( bfs::is_regular_file(filename) ) {
        redux::file::readFile( filename, *this );
        if( nDimensions() != 2 ) {    // not a 2D image
            clear();
        } else {
            if( dimSize(0) != pixels || dimSize(1) != pixels ) {    // size mismatch
                Array<double> tmp = Array<double>::copy(true);
                resize( pixels, pixels );
                redux::image::resize( tmp.get(), tmp.dimSize(0), tmp.dimSize(1), get(), pixels, pixels );
            }
            nPixels = pixels;
            radius = 0;
            normalize();
            generateSupport(1E-9);                         // TODO: tweak or make into a config parameter?
            return true;
        }
    }
    return false;
    
}


void Pupil::generate( uint16_t pixels, double pupilRadius ) {
    
    nPixels = pixels;
    radius = pupilRadius;
    resize( nPixels, nPixels );
    auto ptr = reshape( nPixels, nPixels );        // returns a 2D shared_ptr
    area = makePupil( ptr.get(), nPixels, radius );
    
    normalize();
    generateSupport(1E-9);                         // TODO: tweak or make into a config parameter?
    
}



void Pupil::generateSupport(double threshold){
    
    if(nDimensions() != 2 || dimSize(0) != nPixels || dimSize(1) != nPixels) return;            // init needs to be called first.

    size_t otfPixels = 2*nPixels;
    Array<double> OTF(otfPixels, otfPixels);
    OTF.zero();
    
    Array<double> subOTF(OTF, 0, nPixels-1, 0, nPixels-1);              // bottom-left quadrant of the OTF images
    copy(subOTF);                                                       // copy the pupil
    
    size_t cnt(0);
    area = 0;
    for (auto & value: subOTF) {                                           // find the indices where the pupil is > threshold
        if( value > threshold ) {
            pupilSupport.push_back(cnt);
            size_t otfOffset = (cnt / nPixels) * otfPixels + (cnt % nPixels) + (nPixels / 2) + (nPixels / 2) * otfPixels;
            pupilInOTF.push_back(make_pair (cnt, otfOffset));
            area += value;
        }
        cnt++;
    }
    
    FourierTransform::autocorrelate(OTF);                               // auto-correlate the pupil to generate the support of the OTF.

    double* tmpPtr = OTF.get();
    for (size_t index = 0; index < OTF.nElements(); ++index) {           // map indices where the OTF-mask (auto-correlated pupil-mask) is non-zero.
        if (fabs(tmpPtr[index]) > threshold) {
            otfSupport.push_back(index);
        }
    }
    
}


void Pupil::normalize( void ) {
    
    ArrayStats stats;
    stats.getMinMaxMean(*this);

    if( stats.min != 0.0 || stats.max != 1.0 ) {
        //cerr << "The pupil will be naively re-scaled to the interval [0,1].\n";
    }

    // FIXME:  decide better normalization scheme to allow max values < 1 and min values > 0 (i.e. realistic transmission of an aperture)
    *this -= stats.min;
    if( stats.min == stats.max ) {
        //cerr << "The pupil is a constant value.\n";
    } else {
        *this *= 1.0/(stats.max-stats.min);
    }
    
    

}


void Pupil::dump( string tag ) const {

    if( nElements() ) {
        Ana::write( tag + ".f0", *this );
        vector<size_t> dims = dimensions();
        Array<uint8_t> support(dims);
        support *= 0;
        uint8_t* ptr = support.get();
        for( const size_t& ind: pupilSupport ) {
            ptr[ind] = 1;
        }
        Ana::write( tag + "_support.f0", support );
        for( size_t& d: dims ) d *= 2;
        support.resize(dims);
        support.zero();
        ptr = support.get();
        for( const size_t& ind: otfSupport ) {
            ptr[ind] = 1;
        }
        Ana::write( tag + "_otfsupport.f0", support );
        support.zero();
        ptr = support.get();
        for( const auto& ind: pupilInOTF ) {
            ptr[ind.second] = 1;
        }
        Ana::write( tag + "_pupilinotf.f0", support );
    }


}


Pupil& Pupil::operator=( const Pupil& rhs ) {
    redux::util::Array<double>::operator=( reinterpret_cast<const redux::util::Array<double>&>(rhs) );
    nPixels = rhs.nPixels;
    radius = rhs.radius;
    area = rhs.area;
    pupilSupport = rhs.pupilSupport;
    otfSupport = rhs.otfSupport;
    pupilInOTF = rhs.pupilInOTF;
    return *this;
}


bool Pupil::operator<(const Pupil& rhs) const {
    if( nPixels == rhs.nPixels ) {
        return (radius < rhs.radius);
    }
    return (nPixels<rhs.nPixels);
}

