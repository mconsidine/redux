#
# Set input data for FindExternal.cmake
#

set( EXT_NAME "OpenCV" )
#set( EXT_DEBUG 1 )

set( EXT_LIB_PREFIX opencv_ )
set( EXT_COMPONENTS calib3d contrib core features2d flann imgproc legacy ml objdetect )

set( EXT_LIBPATH_SUFFIXES "lib${LIB_ARCH}${LIB_SUBDIR}" )
set( EXT_LIB_DEBUG_SUFFIX "d" )

set( EXT_HEADER_FILE "opencv2/opencv.hpp" )
set( EXT_VERSION_FILE "opencv2/core/version.hpp" "cvver.h" )
set( EXT_MAJOR_REGEXP "define CV_VERSION_EPOCH" )
set( EXT_MINOR_REGEXP "define CV_VERSION_MAJOR" )
set( EXT_PATCH_REGEXP "define CV_VERSION_MINOR" )


set( EXT_HELPTEXT "Try your systems equivalent of \"apt-get install libopencv-*-dev\"" )

# Attempt to locate libs/headers automagically
include("${CMAKE_CURRENT_LIST_DIR}/FindExternal.cmake")


appendPaths()
