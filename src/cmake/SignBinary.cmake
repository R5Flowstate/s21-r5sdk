# Authenticode-sign one PE. No-op when R5SDK_SIGN_THUMBPRINT (or the env var)
# is empty so unsigned local builds keep working. Fail the build if a
# thumbprint is set and signing does not succeed.

if( NOT R5SDK_SIGN_THUMBPRINT )
	set( R5SDK_SIGN_THUMBPRINT "$ENV{R5SDK_SIGN_THUMBPRINT}" )
endif()

if( NOT R5SDK_SIGN_THUMBPRINT )
	return()
endif()

string( REPLACE " " "" R5SDK_SIGN_THUMBPRINT "${R5SDK_SIGN_THUMBPRINT}" )

if( NOT R5SDK_SIGN_FILE )
	message( FATAL_ERROR "Authenticode: R5SDK_SIGN_FILE is empty" )
endif()

if( NOT EXISTS "${R5SDK_SIGN_FILE}" )
	message( FATAL_ERROR "Authenticode: missing file ${R5SDK_SIGN_FILE}" )
endif()

if( NOT R5SDK_SIGNTOOL )
	set( R5SDK_SIGNTOOL "$ENV{R5SDK_SIGNTOOL}" )
endif()

if( NOT R5SDK_SIGNTOOL )
	message( FATAL_ERROR "Authenticode: signtool.exe not found (set R5SDK_SIGNTOOL)" )
endif()

if( NOT R5SDK_SIGN_TIMESTAMP_URL )
	set( R5SDK_SIGN_TIMESTAMP_URL "$ENV{R5SDK_SIGN_TIMESTAMP_URL}" )
endif()
if( NOT R5SDK_SIGN_TIMESTAMP_URL )
	set( R5SDK_SIGN_TIMESTAMP_URL "http://timestamp.digicert.com" )
endif()

execute_process(
	COMMAND "${R5SDK_SIGNTOOL}" verify /pa "${R5SDK_SIGN_FILE}"
	RESULT_VARIABLE _already
	OUTPUT_QUIET
	ERROR_QUIET
)
if( _already EQUAL 0 )
	return()
endif()

execute_process(
	COMMAND "${R5SDK_SIGNTOOL}" sign
		/fd SHA256
		/td SHA256
		/tr "${R5SDK_SIGN_TIMESTAMP_URL}"
		/sha1 "${R5SDK_SIGN_THUMBPRINT}"
		/d "R5Flowstate"
		/du "https://play.r5flowstate.org"
		"${R5SDK_SIGN_FILE}"
	RESULT_VARIABLE _rc
	OUTPUT_VARIABLE _out
	ERROR_VARIABLE _err
)
if( NOT _rc EQUAL 0 )
	message( FATAL_ERROR "Authenticode failed (${_rc}) for ${R5SDK_SIGN_FILE}:\n${_out}${_err}" )
endif()
