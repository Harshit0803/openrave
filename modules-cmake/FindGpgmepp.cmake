find_path(Gpgmepp_INCLUDE_DIR NAMES gpgme.h)
find_library(Gpgmepp_LIBRARY NAMES gpgme)

if (Gpgmepp_INCLUDE_DIR AND Gpgmepp_LIBRARY)
    set(Gpgmepp_FOUND TRUE)
    message(STATUS "Found Gpgmepp: ${Gpgmepp_LIBRARY}")
else()
    set(Gpgmepp_FOUND FALSE)
    message(WARNING "Gpgmepp not found.")
endif()

mark_as_advanced(Gpgmepp_INCLUDE_DIR Gpgmepp_LIBRARY)
