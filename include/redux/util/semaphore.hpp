#ifndef REDUX_UTIL_SEMAPHORE_HPP
#define REDUX_UTIL_SEMAPHORE_HPP

#include <chrono>
#include <condition_variable>
#include <mutex>


namespace redux {

    namespace util {
                
        class Semaphore {
            
        public:
            Semaphore( unsigned int );

            void get( void );
            void release( void );
            unsigned int count( void );
            
            void decrease( unsigned int );
            void increase( unsigned int );
            void set( unsigned int );
            void reset( void );

            template< typename R,typename P >
            bool get( const std::chrono::duration<R,P>& timeout ) {
                std::unique_lock< std::mutex > lock(mtx);
                if( !cond.wait_for(lock,timeout,[&]()->bool{ return counter>0; }) ) {
                    return false;
                }
                --counter;
                return true;
            }

            class Scope {
                
            public:
                explicit Scope( Semaphore& );
                ~Scope();
                template< typename R,typename P >
                explicit Scope( Semaphore& s, const std::chrono::duration<R,P>& timeout ) : sem(s) {
                    active = sem.get( timeout );
                }
                Scope( Semaphore& s, const int timeout ) : sem(s) {
                    active = sem.get( std::chrono::duration<int>(timeout) );
                }
                void release( void );
                inline operator bool() const { return active; };
            private:
                Semaphore& sem;
                bool active;
            };
            
        private:
            unsigned int counter;
            unsigned int init;
            std::mutex mtx;
            std::condition_variable cond;

        };


    }  // end namespace util

}  // end namespace redux


#endif // REDUX_UTIL_SEMAPHORE_HPP
