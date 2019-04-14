#ifndef REDUX_UTIL_TRACE_HPP
#define REDUX_UTIL_TRACE_HPP

#include "redux/util/cache.hpp"

#include <typeinfo>

#define TRACE_BT_BUF_SIZE 100

namespace redux {
    
    namespace util {

        namespace trace {
            
            struct BT {
                void *mangled_syms[TRACE_BT_BUF_SIZE];
                std::vector<std::string> syms;
                int nSyms;
                static int max_depth;
                BT(void);
                std::string printBackTrace( size_t indent=0 ) const;
            };

            
        }
        
        void thread_trace( const char* file, int line );
        void thread_untrace( void );
        std::string thread_traces( bool all=false );
        
        class Trace {
            
        public:
            typedef std::function<std::string(void)> string_cb_t;
            typedef std::function<size_t(void)> size_cb_t;
            struct trace_t {
                trace_t() : count(0), totalCount(0) {};
                string_cb_t backtraces;
                string_cb_t stats;
                size_cb_t size;
                size_cb_t count;
                size_t totalCount;
            };
            
            static std::string getBackTraces( void );
            static std::string getStats( void );
            
            static trace_t& addTraceObject( size_t id, string_cb_t stats, string_cb_t bt, size_cb_t cnt, size_cb_t sz );
            static void removeTraceObject( size_t id );
            
            static int maxDepth( void ) { return trace::BT::max_depth; }
            static void setMaxDepth( const int& md ) { trace::BT::max_depth = std::max(0,md); }

        private:
            static std::map<size_t, trace_t> traces;
            static std::mutex mtx;

        };
        
        
        template <typename T>
        class TraceObject {
            
            typedef std::function<size_t(T*)> size_cb_t;
            typedef const std::pair<const T*,Trace*> cache_arg_t;
            typedef std::function<void(cache_arg_t&)> cache_func_t;

            trace::BT bt;
            
        public:
            
            
            TraceObject() {
                Cache::get<T*,Trace*>(reinterpret_cast<T*>(this));
                static Trace::trace_t& tt = Trace::addTraceObject( Cache::getID1<T*,Trace*>(),
                                      std::bind(TraceObject<T>::getStats),
                                      std::bind(TraceObject<T>::printBackTraces),
                                      std::bind(TraceObject<T>::count),
                                      std::bind(TraceObject<T>::getTotalSize)
                );
                tt.totalCount++;
            }

            virtual ~TraceObject() { stopTrace(); Trace::removeTraceObject( Cache::getID1<T*,Trace*>()); }
            void stopTrace( void ) { Cache::erase<T*,Trace*>(reinterpret_cast<T*>(this)); }
            static size_t count( void ) { return Cache::size<T*,Trace*>(); }
            static size_t getTotalSize( void ) {
                size_t sz(0);
                cache_func_t func = [&](cache_arg_t& p) { if(p.first) sz += p.first->size(); };
                Cache::get().for_each<T*,Trace*>( func );
                return sz;
            }

            //virtual std::string trace_info( void ) const;
            
            static std::string getStats( void ) { return Cache::get().stats<T*,Trace*>(); }
            std::string printBackTrace( size_t indent=5 ) const { return bt.printBackTrace(indent); }
            static std::string printBackTraces( void ) {
                std::string nm = "  " + Cache::getName<T*,Trace*>() + "\n";
                std::string ret;
                //auto btfunc = std::bind(TraceObject<T>::printBackTrace, std::placeholders::_1);
                cache_func_t func = [&](cache_arg_t& p) {
                    if( p.first ) {
                        ret += hexString(p.first) + nm + p.first->bt.printBackTrace(5);
                    }
                };
                Cache::get().for_each<T*,Trace*>( func );
                return ret;
            }

            virtual inline size_t size( void ) const { return sizeof(T); }
                   
        protected:

            struct trace_data_t {
                trace::BT bt;
            } trace_d;

        };

    }   // util

}   // redux

#ifdef TRACE_THREADS
#define THREAD_MARK redux::util::thread_trace(__FILE__,__LINE__)
#define THREAD_UNMARK redux::util::thread_untrace()
#else
#define THREAD_MARK 
#define THREAD_UNMARK 
#endif

#endif  // REDUX_UTIL_TRACE_HPP







