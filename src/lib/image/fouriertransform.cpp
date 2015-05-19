#include "redux/image/fouriertransform.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

using namespace redux::image;
using namespace redux::util;
using namespace redux;
using namespace std;


FourierTransform::Plan::Index::Index (const std::vector<size_t>& dims, TYPE t, uint8_t nt) : tp (t), nThreads (nt), sizes (dims) {
    sizes.erase( remove_if( sizes.begin(), sizes.end(), [](size_t i){ return i <= 1;}), sizes.end());
    if ( sizes.empty() ) {
        throw std::logic_error ("FT::Plan constructed with no non-trivial dimensions:  " + printArray (dims, "in"));
    }
}

bool FourierTransform::Plan::Index::operator< (const Index& rhs) const {

    if (tp == rhs.tp) {
        if (nThreads == rhs.nThreads) {
            return (sizes < rhs.sizes);
        } else return (nThreads < rhs.nThreads);
    } else return tp < rhs.tp;

}



FourierTransform::Plan::Plan ( const Index& i ) : id(i) {
    init();
}


FourierTransform::Plan::~Plan() {
    if (! id.sizes.empty()) {
        fftw_destroy_plan (forward_plan);
        fftw_destroy_plan (backward_plan);
    }
}


void FourierTransform::Plan::init (void) {
    
    //cout << "Generating plan with size = (" << id.sizes[0] << "," << id.sizes[1] << ")  nT = " << (int)id.nThreads << endl;
    
    fftw_plan_with_nthreads(id.nThreads);
    
    if (id.tp == R2C) {
        if (id.sizes.size() == 2) {
            auto in = sharedArray<double> (id.sizes[0], id.sizes[1]);
            auto out = sharedArray<fftw_complex> (id.sizes[0], id.sizes[1] / 2 + 1);
            forward_plan = fftw_plan_dft_r2c_2d (id.sizes[0], id.sizes[1], *in.get(), *out.get(), FFTW_MEASURE);
            backward_plan = fftw_plan_dft_c2r_2d (id.sizes[0], id.sizes[1], *out.get(), *in.get(), FFTW_MEASURE);
        } else
            if (id.sizes.size() == 1) {
                auto in = sharedArray<double> (id.sizes[0]);
                auto out = sharedArray<fftw_complex> (id.sizes[0] / 2 + 1);
                forward_plan = fftw_plan_dft_r2c_1d (id.sizes[0], in.get(), out.get(), FFTW_MEASURE);
                backward_plan = fftw_plan_dft_c2r_1d (id.sizes[0] / 2 + 1, out.get(), in.get(), FFTW_MEASURE);
            } else {
                throw std::logic_error ("FT::Plan::init() is only implemented for 1/2 dimensions, add more when/if needed: " + printArray (id.sizes, "dims"));
            }
    } else {
        if (id.tp == C2C)  {
            if (id.sizes.size() == 2) {
                auto in = sharedArray<fftw_complex> (id.sizes[0], id.sizes[1]);
                auto out = sharedArray<fftw_complex> (id.sizes[0], id.sizes[1]);
                forward_plan = fftw_plan_dft_2d (id.sizes[0], id.sizes[1], *in.get(), *out.get(), FFTW_FORWARD, FFTW_MEASURE);
                backward_plan = fftw_plan_dft_2d (id.sizes[0], id.sizes[1], *in.get(), *out.get(), FFTW_BACKWARD, FFTW_MEASURE);
            } else {
                if (id.sizes.size() == 1) {
                    auto in = sharedArray<fftw_complex> (id.sizes[0]);
                    auto out = sharedArray<fftw_complex> (id.sizes[0]);
                    forward_plan = fftw_plan_dft_1d (id.sizes[0], in.get(), out.get(), FFTW_FORWARD, FFTW_MEASURE);
                    backward_plan =  fftw_plan_dft_1d (id.sizes[0], in.get(), out.get(), FFTW_BACKWARD, FFTW_MEASURE);
                } else {
                    throw std::logic_error ("FT::Plan::init() is only implemented for 1/2 dimensions, add more when/if needed: " + printArray (id.sizes, "dims"));
                }
            }
        } else {
            throw std::logic_error ("FT::Plan::init() unknown tp.");
        }
    }
}


namespace {
    struct PlanCompare {
        bool operator() (const FourierTransform::Plan::Ptr &a, const FourierTransform::Plan::Ptr &b) const {
            return (*a < *b);
        }
    };
    struct InitCleanup {  // using a static instance below ensures that fftw_init_threads is called only once, and cleanup is done when program exits.
        InitCleanup(){ /*cout << "InitCleanup()" << endl;*/ fftw_init_threads(); };
        ~InitCleanup(){ /*cout << "~InitCleanup()" << endl;*/ fftw_cleanup_threads(); };
    };
}


FourierTransform::Plan::Ptr FourierTransform::getPlan (const std::vector<size_t>& dims, Plan::TYPE tp, uint8_t nThreads) {
    static std::map<Plan::Index, Plan::Ptr> plans;
    static InitCleanup icSingleton;
    static mutex mtx;
    unique_lock<mutex> lock (mtx);
    auto plan = plans.emplace (Plan::Index(dims, tp, nThreads), nullptr);

    if (plan.second) {  // insertion successful, i.e. new plan -> initialize it
        plan.first->second.reset (new Plan (plan.first->first));
    }

    return plan.first->second;
}


FourierTransform::FourierTransform() : centered (false), halfComplex (false), normalized (false) {

}


FourierTransform::FourierTransform (size_t ySize, size_t xSize, int flags, uint8_t nT) :
    centered (false), halfComplex (! (flags & FT_FULLCOMPLEX)), normalized (false), nThreads(nT) {
    if (halfComplex) {
        Array<complex_t>::resize (ySize, xSize / 2 + 1);
    } else {
        Array<complex_t>::resize (ySize, xSize);
    }
}


FourierTransform::FourierTransform (const FourierTransform& rhs) : plan (rhs.plan), centered (rhs.centered),
    halfComplex (rhs.halfComplex), normalized (rhs.normalized), nThreads (rhs.nThreads), inputSize (rhs.inputSize) {
    rhs.copy (*this);
}


template <typename T>
FourierTransform::FourierTransform (const Array<T>& rhs, int flags, uint8_t nT) :
    centered (false), halfComplex (false), normalized (false) {

    reset (rhs, flags, nT);

}
template FourierTransform::FourierTransform (const Array<int16_t>&, int, uint8_t);
template FourierTransform::FourierTransform (const Array<int32_t>&, int, uint8_t);
template FourierTransform::FourierTransform (const Array<float>&, int, uint8_t);
template FourierTransform::FourierTransform (const Array<double>&, int, uint8_t);
template FourierTransform::FourierTransform (const Array<complex_t>&, int, uint8_t);


template <typename T>
void FourierTransform::reset (const Array<T>& rhs, int flags, uint8_t nT) {

    vector<size_t> dims = rhs.dimensions();

    if (dims.empty()) {
        throw logic_error ("FourierTransform::reset() called with no non-trivial dimensions: " + printArray (rhs.dimensions(), "dims"));
    } else {
        if (dims.size() > 2) {
            throw logic_error ("FourierTransform::reset() only supports 1&2 dimensions at the moment: " + printArray (dimensions(), "dims"));
        }
    }

    inputSize = rhs.nElements();
    normalized = false;
    centered = false;       // output from FFTW is in re-ordered form, i.e. not centered
    nThreads = nT;

    if (flags & FT_FULLCOMPLEX) {
        halfComplex = false;
        Array<complex_t> tmp;
        rhs.copy (tmp);

        if (flags & FT_REORDER) {
            reorder (tmp);
        }

        init (tmp);
    } else {
        halfComplex = true;     // transform of real data, let's save half the space.
        Array<double> tmp;
        rhs.copy (tmp);

        if (flags & FT_REORDER) {
            reorder (tmp);
        }

        init (tmp);
    }

    if (flags & FT_NORMALIZE) {
        normalize();
    }

}


namespace redux {
    namespace image {
        template <>
        void FourierTransform::reset (const Array<complex_t>& rhs, int flags, uint8_t nT) {

            vector<size_t> dims = rhs.dimensions();

            if (dims.empty()) {
                throw logic_error ("FourierTransform::reset() called with no non-trivial dimensions: " + printArray (rhs.dimensions(), "dims"));
            } else {
                if (dims.size() > 2) {
                    throw logic_error ("FourierTransform::reset() only supports 1&2 dimensions at the moment: " + printArray (dimensions(), "dims"));
                }
            }

            inputSize = rhs.nElements();
            normalized = false;
            centered = false;       // output from FFTW is in re-ordered form, i.e. not centered
            halfComplex = false;
            nThreads = nT;

            Array<complex_t> tmp;
            rhs.copy (tmp);

            if (flags & FT_REORDER) {
                reorder();
            }

            init (tmp);

            if (flags & FT_NORMALIZE) {
                normalize();
            }

        }
    }
}
template void FourierTransform::reset (const Array<int16_t>&, int, uint8_t);
template void FourierTransform::reset (const Array<int32_t>&, int, uint8_t);
template void FourierTransform::reset (const Array<float>&, int, uint8_t);
template void FourierTransform::reset (const Array<double>&, int, uint8_t);
template void FourierTransform::reset (const Array<complex_t>&, int, uint8_t);


void FourierTransform::set(Array<double>& rhs) {
    fftw_execute_dft_r2c (plan->forward_plan, rhs.ptr(), reinterpret_cast<fftw_complex*> (ptr()));
}


void FourierTransform::set(Array<complex_t>& rhs) {
    fftw_execute_dft (plan->forward_plan, reinterpret_cast<fftw_complex*> (rhs.ptr()), reinterpret_cast<fftw_complex*> (ptr()));
}


void FourierTransform::init (const Array<double>& rhs) {

    const vector<size_t>& dims = rhs.dimensions();

    // for r2c transforms, the last dimension has size = n/2+1
    if( (dims[0] != dimSize(0)) || (2*(dims[1]-1) != 2*(dimSize(1)-1))) {
        Array<complex_t>::resize (dims[0],dims[1]/2+1);
    }

    plan = getPlan (rhs.dimensions(), Plan::R2C, nThreads);
    size_t sz = rhs.nElements();
    double* tmpData = new double[sz];
    memcpy (tmpData, rhs.ptr(), sz * sizeof (double));  // copy data because r2c-transforms modifies input
    fftw_execute_dft_r2c (plan->forward_plan, tmpData, reinterpret_cast<fftw_complex*> (ptr()));
    delete[] tmpData;

}


void FourierTransform::init (const Array<complex_t>& rhs) {
    if (!sameSizes (rhs)) {
        Array<complex_t>::resize (rhs.dimensions());
    }

    plan = getPlan (rhs.dimensions(), Plan::C2C, nThreads);
    complex_t* dataPtr = const_cast<complex_t*> (rhs.ptr());    // fftw takes non-const, even if input is not modified.
    fftw_execute_dft (plan->forward_plan, reinterpret_cast<fftw_complex*> (dataPtr), reinterpret_cast<fftw_complex*> (ptr()));
}


template <typename T>
void FourierTransform::directInverse (Array<T>& out) {

    if (centered) {
        reorder();
    }

    if (halfComplex) {
        fftw_execute_dft_c2r (plan->backward_plan, reinterpret_cast<fftw_complex*> (ptr()), reinterpret_cast<double*> (out.ptr()));
    } else {
        fftw_execute_dft (plan->backward_plan, reinterpret_cast<fftw_complex*> (ptr()), reinterpret_cast<fftw_complex*> (out.ptr()));
    }

    if (!normalized) {
        out *= 1.0/inputSize;
    }

}
template void FourierTransform::directInverse (Array<double>& out);
template void FourierTransform::directInverse (Array<complex_t>& out);


template <typename T>
void FourierTransform::inv (Array<T>& out, int flags) const {
    vector<size_t> dims = dimensions();
    if (halfComplex) {
        dims.back() -= 1;
        dims.back() <<= 1;      // for r2c transforms, the last dimension has half size (n/2+1)
        Array<double> tmp(dims);
        size_t sz = nElements();
        complex_t* dataPtr = new complex_t[sz];
        memcpy (dataPtr, ptr(), sz * sizeof (complex_t));  // copy since r2c transforms modifies input
        fftw_execute_dft_c2r (plan->backward_plan, reinterpret_cast<fftw_complex*> (dataPtr), tmp.ptr());
        delete[] dataPtr;
        tmp.copy (out);
    } else {
        Array<complex_t> tmp(dims);
        complex_t* dataPtr = const_cast<complex_t*> (ptr());
        fftw_execute_dft (plan->backward_plan, reinterpret_cast<fftw_complex*> (dataPtr), reinterpret_cast<fftw_complex*> (tmp.ptr()));
        out = tmp;
        //tmp.copy( out );
    }

    if (flags & FT_REORDER) {
        reorder (out);
    }

}
namespace redux {
    namespace image {
        template <>
        void FourierTransform::inv (Array<double>& out, int flags) const {
           const vector<size_t>& dims = dimensions();
            if (halfComplex) {
               if( (dims[0] != out.dimSize(0)) || (2*(dims[1]-1) != out.dimSize(1))) {
                    out.resize(dims[0],2*(dims[1]-1));
                }
                size_t sz = nElements();
                complex_t* dataPtr = new complex_t[sz];
                memcpy (dataPtr, ptr(), sz * sizeof (complex_t));  // copy since r2c transforms modifies input
                fftw_execute_dft_c2r (plan->backward_plan, reinterpret_cast<fftw_complex*> (dataPtr), out.ptr());
                delete[] dataPtr;
            } else {
                Array<complex_t> tmp(dims);
                complex_t* dataPtr = const_cast<complex_t*> (ptr());
                fftw_execute_dft (plan->backward_plan, reinterpret_cast<fftw_complex*> (dataPtr), reinterpret_cast<fftw_complex*> (tmp.ptr()));
                out = tmp;
            }

            if (flags & FT_REORDER) {
                reorder (out);
            }

        }
    }
}
template void FourierTransform::inv (Array<float>& out, int flags) const;
template void FourierTransform::inv (Array<double>& out, int flags) const;
template void FourierTransform::inv (Array<complex_t>& out, int flags) const;


template <typename T>
Array<T> FourierTransform::correlate (const Array<T>& in) const {
    int f = FT_REORDER;

    if (centered) {
        f = 0;
    }

    FourierTransform inFT (in, f);
    auto it = inFT.begin();

    for (auto & it2 : *this) {
        *it++ *= conj (it2);
    }

    Array<T> out (in.dimensions());
    inFT.inv (out, FT_REORDER);
    return out;

}
template Array<float> FourierTransform::correlate (const Array<float>&) const;
template Array<double> FourierTransform::correlate (const Array<double>&) const;
template Array<complex_t> FourierTransform::correlate (const Array<complex_t>&) const;


void FourierTransform::autocorrelate (void) {
    for (auto & it : *this) {
        it = norm (it);         // = re*re + im*im
    }
}


template <typename T>
void FourierTransform::autocorrelate (Array<T>& data) {
    FourierTransform ft (data);

    for (auto & it : ft) {
        it = norm (it);
    }

    ft.inv (data, FT_REORDER);
}
template void FourierTransform::autocorrelate (Array<float>&);
template void FourierTransform::autocorrelate (Array<double>&);
template void FourierTransform::autocorrelate (Array<complex_t>&);

template <typename T>
void FourierTransform::autocorrelate (const Array<T>& in, Array<T>& out) {
    FourierTransform ft (in);

    for (auto & it : ft) {
        it = norm (it);
    }

    out.resize (in.dimensions());
    ft.inv (out, FT_REORDER);
}
template void FourierTransform::autocorrelate (const Array<float>&, Array<float>&);
template void FourierTransform::autocorrelate (const Array<double>&, Array<double>&);
template void FourierTransform::autocorrelate (const Array<complex_t>&, Array<complex_t>&);


Array<double> FourierTransform::power (void) const {
    Array<double> tmp (dimensions());
    auto it = tmp.begin();

    for (auto & it2 : *this) {
        *it++ = norm (it2);
    }

    return tmp;
}


double FourierTransform::noise (int mask, double limit) const {

    Array<double> pwr = power();

    double noise_power = 0.0;

    int N = 0;
    double xx, yy;
    int npY = dimSize (0);
    int npX = dimSize (1);

    if (mask < 0) {         // specifying mask < 0 will match MvN's noise_level computation   ( mask = (np/6) )
        mask = std::max (npY, npX) / 6 + 1;
    }

    if (limit < 0) {        // specifying limit < 0 will match MvN's noise_level computation  ( sqrt((npY*npX)/4.0) )
        if (halfComplex) {
            limit = sqrt ( (npY * (npX - 1)) / 2.0);
        } else  {
            limit = sqrt ( (npY * npX) / 4.0);
        }
    }

    double limits = limit * limit;
    int endX = npX;
    int endY = npY;

    if (mask > 0) {
        endY = npY - mask + 1;

        if (!halfComplex) {
            endX = npX - mask + 1;
        }
    }

    for (int x = mask; x < endX; ++x) {
        if (halfComplex) {
            xx = x;
        } else {
            xx = std::min (x, npX - x);
        }

        xx *= xx;
        int count = 1 + (halfComplex && x > 0 && x < npX - 1); // if halfComplex and x is not first/last column, value is counted twice.

        for (int y = mask; y < endY; ++y) {
            yy = std::min (y, npY - y);

            if ( (xx + yy * yy) < limits) continue;         // inside region to ignore

            noise_power += count * pwr (y, x);
            N += count;

        }
    }

    return std::sqrt (noise_power / (N * inputSize)); // normalize both by elementcount and total elements
    // (because the power-spectrum was not normalized first)

}



template <typename T>
void FourierTransform::convolveInPlace (Array<T>& inout, int flags) const {

    auto dims = inout.dimensions();

    if (halfComplex) {            // for half-complex, the last dimension has half size (n/2+1)
        dims.back() >>= 1;
        dims.back() += 1;
    }

    if (dims != dimensions()) {
        cout << "FourierTransform::convolveInPlace(): input dimensions does not match this FourierTransform.\n\t"
             << printArray (dims, "input-dims") << printArray (dimensions(), "   FT-dims") << endl;
        return;
    }

    if (! normalized) {             // normalize only 1 of the inputs
        flags |= FT_NORMALIZE;
    }

    FourierTransform inFT (inout, flags, nThreads);
    inFT *= *this;
    inFT.inv (inout, FT_REORDER);
}
template void FourierTransform::convolveInPlace (Array<float>& inout, int flags) const;
template void FourierTransform::convolveInPlace (Array<double>& inout, int flags) const;
template void FourierTransform::convolveInPlace (Array<complex_t>& inout, int flags) const;


void FourierTransform::normalize (FourierTransform& in) {
    if (in.normalized) return;

    in *= (1.0 / (in.inputSize));
    in.inputSize = 1;
    in.normalized = true;
}


template <typename T>
void FourierTransform::reorder (redux::util::Array<T>& in) {

    size_t halfY = in.dimSize (0) / 2;
    size_t stride = in.dimSize (1);
    size_t halfX = stride / 2;
    size_t chunkSize = halfX * sizeof (T);
    char *buf = new char[chunkSize];

    T* southWest = in.ptr();
    T* southEast = in.ptr (0, halfX);
    T* northWest = in.ptr (halfY, 0);
    T* northEast = in.ptr (halfY, halfX);

    for (size_t y = 0; y < halfY; ++y) {
        memcpy (buf, southWest, chunkSize);
        memcpy (southWest, northEast, chunkSize);
        memcpy (northEast, buf, chunkSize);
        southWest += stride;
        northEast += stride;
        memcpy (buf, southEast, chunkSize);
        memcpy (southEast, northWest, chunkSize);
        memcpy (northWest, buf, chunkSize);
        southEast += stride;
        northWest += stride;
    }

    delete[] buf;

}
template void FourierTransform::reorder (redux::util::Array<float>& in);
template void FourierTransform::reorder (redux::util::Array<double>& in);
template void FourierTransform::reorder (redux::util::Array<complex_t>& in);


void FourierTransform::reorder (void) {
    if (!halfComplex) {
        reorder (*this);
        centered = !centered;
    } else {
        cout << "REORDER:  fix for half-complex." << endl;
    }
}


FourierTransform FourierTransform::reordered (void) const {
    FourierTransform tmp (*this);
    tmp.reorder();
    return tmp;
}


void FourierTransform::resize (const std::vector<size_t>& sizes) {

    Array<complex_t>::resize (sizes);

    if (halfComplex) {
        plan = getPlan (sizes, Plan::R2C, nThreads);
    } else {
        plan = getPlan (sizes, Plan::C2C, nThreads);
    }

}


const FourierTransform& FourierTransform::operator*= (const FourierTransform& rhs) {

    if (centered != rhs.centered) {
        *this *= rhs.reordered();
        return *this;
    }

    if (halfComplex == rhs.halfComplex) {
        //Array<complex_t>::operator*= (rhs);
        //std::transform (first, first+5, second, results, std::multiplies<int>());
        std::transform (ptr(), ptr()+nElements(), rhs.ptr(), ptr(), std::multiplies<complex_t>());
    } else if (halfComplex) {
        Array<complex_t> half (reinterpret_cast<const Array<complex_t>&> (rhs), dimensions());
        Array<complex_t>::operator*= (half);
    } else {
        Array<complex_t> half (reinterpret_cast<Array<complex_t>&> (*this), rhs.dimensions());

        int d0 = dimSize (0);
        int d1 = dimSize (1);
        int d2 = rhs.dimSize (1);
        int midpoint = d1 - d2 + 1; // works for both odd & even dimension sizes
        complex_t** thisPtr = redux::util::makePointers (ptr(), d0, d1);
        const complex_t** rhsPtr = redux::util::makePointers (rhs.ptr(), d0, d2);

        for (int y = 0; y < d0; ++y) {
            for (int x = 0; x < d2; ++x) {
                thisPtr[y][x] *= rhsPtr[y][x];
                if (x && x < midpoint) thisPtr[y][d1 - x] *= rhsPtr[y][x];
            }
        }

        redux::util::delPointers (thisPtr);
        redux::util::delPointers (rhsPtr);
    }

    return *this;
}


const FourierTransform& FourierTransform::operator= (const FourierTransform& rhs) {
    Array<complex_t>::operator= (rhs);
    plan = rhs.plan;
    centered = rhs.centered;
    halfComplex = rhs.halfComplex;
    normalized = rhs.normalized;
    inputSize = rhs.inputSize;
    return *this;
}

