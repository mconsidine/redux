#ifndef REDUX_NETWORK_PROTOCOL_HPP
#define REDUX_NETWORK_PROTOCOL_HPP

#include <cstdint>

namespace redux {

    namespace network {

        enum Command : uint8_t { CMD_OK = 0,       // NB: specific-type enums are supported by gcc/clang, but may not behave well with other compilers
                                 CMD_CONNECT,      //   check this !!! /THI
                                 CMD_ADD_JOB,
                                 CMD_MOD_JOB,
                                 CMD_DEL_JOB,
                                 CMD_GET_WORK,
                                 CMD_GET_JOBLIST,
                                 CMD_PUT_PARTS,
                                 CMD_STAT,
                                 CMD_JSTAT,
                                 CMD_PSTAT,
                                 CMD_SLV_CFG,
                                 CMD_SLV_IO,
                                 CMD_SLV_RES,
                                 CMD_SLV_REJ,
                                 CMD_DEL_SLV,
                                 CMD_AUTH,
                                 CMD_CFG,
                                 CMD_DISCONNECT,
                                 CMD_LOG_CONNECT,
                                 CMD_RESET,
                                 CMD_DIE,
                                 CMD_ERR = 255
                               };
    }   // network
}   // redux

#endif // REDUX_NETWORK_PROTOCOL_HPP
