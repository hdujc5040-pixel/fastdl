// fastdl - multi-connection HTTP/HTTPS downloader for Windows (WinHTTP, no third-party dependencies)
//
//   fastdl <url> [-o file] [-t threads] [-c chunk_mb] [-H "Name: value"]...
//
// The file is split into chunks that N connections fetch in parallel with Range requests. Progress is shown like a
// browser does (bar, percent, size, speed, ETA, connections). Interrupted downloads resume from <file>.fdl.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment( lib, "winhttp.lib" )

#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif

namespace
{
using clk = std::chrono::steady_clock;

constexpr wchar_t k_version[] = L"1.0.0";

std::atomic< bool > g_abort { false };
std::atomic< bool > g_fatal { false };
std::atomic< uint64_t > g_downloaded { 0 };
std::mutex g_msg_mutex;
std::wstring g_fatal_msg;
bool g_vt = false;
bool g_console = false;

void fail( std::wstring const& message )
{
    std::lock_guard< std::mutex > lock( g_msg_mutex );
    if ( !g_fatal.exchange( true ) )
        g_fatal_msg = message;
}

BOOL WINAPI on_ctrl( DWORD type )
{
    if ( type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT )
    {
        g_abort = true;
        return TRUE;
    }
    return FALSE;
}

void out( std::wstring const& text )
{
    std::fputws( text.c_str( ), stdout );
    std::fflush( stdout );
}

std::wstring fmt_size( double bytes )
{
    wchar_t const* const units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int i = 0;
    while ( bytes >= 1024.0 && i < 4 )
    {
        bytes /= 1024.0;
        ++i;
    }

    wchar_t buf[ 64 ];
    std::swprintf( buf, 64, i == 0 ? L"%.0f %ls" : L"%.2f %ls", bytes, units[ i ] );
    return buf;
}

std::wstring fmt_time( double seconds )
{
    if ( seconds < 0 || seconds > 99.0 * 3600 )
        return L"--:--";

    auto const s = static_cast< long long >( seconds );
    wchar_t buf[ 32 ];
    if ( s >= 3600 )
        std::swprintf( buf, 32, L"%lld:%02lld:%02lld", s / 3600, ( s / 60 ) % 60, s % 60 );
    else
        std::swprintf( buf, 32, L"%02lld:%02lld", s / 60, s % 60 );
    return buf;
}

// ---------------------------------------------------------------- url / headers
struct url_t
{
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
};

bool crack_url( std::wstring const& text, url_t& url )
{
    URL_COMPONENTS c { };
    c.dwStructSize = sizeof c;
    c.dwSchemeLength = c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = static_cast< DWORD >( -1 );
    if ( !WinHttpCrackUrl( text.c_str( ), 0, 0, &c ) )
        return false;
    if ( c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS )
        return false;

    url.host.assign( c.lpszHostName, c.dwHostNameLength );
    url.path.assign( c.lpszUrlPath, c.dwUrlPathLength );
    if ( c.lpszExtraInfo && c.dwExtraInfoLength )
        url.path.append( c.lpszExtraInfo, c.dwExtraInfoLength );
    if ( url.path.empty( ) )
        url.path = L"/";
    url.port = c.nPort;
    url.https = c.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

std::wstring query_header( HINTERNET request, DWORD level )
{
    DWORD size = 0;
    WinHttpQueryHeaders( request, level, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX );
    if ( GetLastError( ) != ERROR_INSUFFICIENT_BUFFER || !size )
        return { };

    std::wstring value( size / sizeof( wchar_t ) + 1, L'\0' );
    if ( !WinHttpQueryHeaders( request, level, WINHTTP_HEADER_NAME_BY_INDEX, value.data( ), &size, WINHTTP_NO_HEADER_INDEX ) )
        return { };

    value.resize( size / sizeof( wchar_t ) );
    return value;
}

DWORD query_status( HINTERNET request )
{
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX );
    return status;
}

std::wstring utf8_to_wide( std::string const& s )
{
    if ( s.empty( ) )
        return { };
    int const n = MultiByteToWideChar( CP_UTF8, 0, s.data( ), static_cast< int >( s.size( ) ), nullptr, 0 );
    std::wstring w( n, L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, s.data( ), static_cast< int >( s.size( ) ), w.data( ), n );
    return w;
}

std::string wide_to_utf8( std::wstring const& w )
{
    if ( w.empty( ) )
        return { };
    int const n = WideCharToMultiByte( CP_UTF8, 0, w.data( ), static_cast< int >( w.size( ) ), nullptr, 0, nullptr, nullptr );
    std::string s( n, '\0' );
    WideCharToMultiByte( CP_UTF8, 0, w.data( ), static_cast< int >( w.size( ) ), s.data( ), n, nullptr, nullptr );
    return s;
}

std::wstring percent_decode( std::wstring const& text )
{
    std::string bytes = wide_to_utf8( text ), decoded;
    for ( size_t i = 0; i < bytes.size( ); ++i )
    {
        if ( bytes[ i ] == '%' && i + 2 < bytes.size( ) && std::isxdigit( static_cast< unsigned char >( bytes[ i + 1 ] ) ) && std::isxdigit( static_cast< unsigned char >( bytes[ i + 2 ] ) ) )
        {
            decoded.push_back( static_cast< char >( std::stoi( bytes.substr( i + 1, 2 ), nullptr, 16 ) ) );
            i += 2;
        }
        else
            decoded.push_back( bytes[ i ] );
    }
    return utf8_to_wide( decoded );
}

std::wstring sanitize( std::wstring name )
{
    for ( wchar_t& ch : name )
        if ( ch < 32 || std::wcschr( L"\\/:*?\"<>|", ch ) )
            ch = L'_';

    while ( !name.empty( ) && ( name.back( ) == L'.' || name.back( ) == L' ' ) )
        name.pop_back( );
    return name;
}

std::wstring name_from_disposition( std::wstring const& header )
{
    auto const star = header.find( L"filename*=" );
    if ( star != std::wstring::npos )
    {
        std::wstring v = header.substr( star + 10 );
        v = v.substr( 0, v.find( L';' ) );
        auto const q = v.find( L"''" );
        if ( q != std::wstring::npos )
            return percent_decode( v.substr( q + 2 ) );
    }

    auto const plain = header.find( L"filename=" );
    if ( plain == std::wstring::npos )
        return { };

    std::wstring v = header.substr( plain + 9 );
    if ( !v.empty( ) && v[ 0 ] == L'"' )
    {
        v = v.substr( 1 );
        v = v.substr( 0, v.find( L'"' ) );
    }
    else
        v = v.substr( 0, v.find( L';' ) );
    return v;
}

std::wstring name_from_url( std::wstring path )
{
    path = path.substr( 0, path.find_first_of( L"?#" ) );
    auto const slash = path.find_last_of( L'/' );
    if ( slash != std::wstring::npos )
        path = path.substr( slash + 1 );
    return percent_decode( path );
}

// ---------------------------------------------------------------- probe
struct probe_t
{
    bool ok = false;
    bool ranges = false;
    uint64_t total = 0; // 0 = unknown
    std::wstring final_url;
    std::wstring name;
    std::wstring error;
};

probe_t probe( HINTERNET session, url_t const& url, std::wstring const& extra )
{
    probe_t result;
    HINTERNET const connect = WinHttpConnect( session, url.host.c_str( ), url.port, 0 );
    if ( !connect )
    {
        result.error = L"cannot connect";
        return result;
    }

    HINTERNET const request = WinHttpOpenRequest( connect, L"GET", url.path.c_str( ), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.https ? WINHTTP_FLAG_SECURE : 0 );
    if ( request )
    {
        std::wstring const headers = extra + L"Range: bytes=0-0\r\n";
        if ( WinHttpSendRequest( request, headers.c_str( ), static_cast< DWORD >( -1 ), nullptr, 0, 0, 0 ) && WinHttpReceiveResponse( request, nullptr ) )
        {
            DWORD const status = query_status( request );
            if ( status == 206 )
            {
                // Content-Range: bytes 0-0/12345
                std::wstring const range = query_header( request, WINHTTP_QUERY_CONTENT_RANGE );
                auto const slash = range.find( L'/' );
                if ( slash != std::wstring::npos && range.compare( slash + 1, 1, L"*" ) != 0 )
                    result.total = std::wcstoull( range.c_str( ) + slash + 1, nullptr, 10 );
                result.ranges = result.total > 0;
                result.ok = true;
            }
            else if ( status == 200 )
            {
                std::wstring const length = query_header( request, WINHTTP_QUERY_CONTENT_LENGTH );
                result.total = length.empty( ) ? 0 : std::wcstoull( length.c_str( ), nullptr, 10 );
                result.ranges = false;
                result.ok = true;
            }
            else if ( status == 416 )
            {
                result.total = 0;
                result.ok = true;
            }
            else
                result.error = L"server answered HTTP " + std::to_wstring( status );

            if ( result.ok )
            {
                DWORD size = 0;
                WinHttpQueryOption( request, WINHTTP_OPTION_URL, nullptr, &size );
                if ( size )
                {
                    std::wstring final_url( size / sizeof( wchar_t ) + 1, L'\0' );
                    if ( WinHttpQueryOption( request, WINHTTP_OPTION_URL, final_url.data( ), &size ) )
                    {
                        final_url.resize( size / sizeof( wchar_t ) );
                        result.final_url = final_url;
                    }
                }

                std::wstring name = name_from_disposition( query_header( request, WINHTTP_QUERY_CONTENT_DISPOSITION ) );
                result.name = name;
            }
        }
        else
            result.error = L"request failed (error " + std::to_wstring( GetLastError( ) ) + L")";
        WinHttpCloseHandle( request );
    }
    else
        result.error = L"cannot open request";

    WinHttpCloseHandle( connect );
    return result;
}

// ---------------------------------------------------------------- download
enum class fetch_t { ok, retry, fatal };

// one request for [pos, hi] (ranged) or the whole body (not ranged); pos advances as data is written
fetch_t fetch( HINTERNET connect, url_t const& url, std::wstring const& extra, uint64_t& pos, uint64_t hi, uint64_t known_total, HANDLE file, bool ranged )
{
    HINTERNET const request = WinHttpOpenRequest( connect, L"GET", url.path.c_str( ), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.https ? WINHTTP_FLAG_SECURE : 0 );
    if ( !request )
        return fetch_t::retry;

    std::wstring headers = extra;
    if ( ranged )
        headers += L"Range: bytes=" + std::to_wstring( pos ) + L"-" + std::to_wstring( hi ) + L"\r\n";

    fetch_t result = fetch_t::retry;
    do
    {
        if ( !WinHttpSendRequest( request, headers.empty( ) ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str( ), headers.empty( ) ? 0 : static_cast< DWORD >( -1 ), nullptr, 0, 0, 0 ) )
            break;
        if ( !WinHttpReceiveResponse( request, nullptr ) )
            break;

        DWORD const status = query_status( request );
        if ( ranged ? status != 206 : status != 200 )
        {
            if ( status >= 500 || status == 429 || status == 408 )
                break;
            fail( L"server answered HTTP " + std::to_wstring( status ) );
            result = fetch_t::fatal;
            break;
        }

        std::vector< uint8_t > buffer( 256 * 1024 );
        bool broken = false;
        for ( ;; )
        {
            if ( g_abort || g_fatal )
                break;

            DWORD got = 0;
            if ( !WinHttpReadData( request, buffer.data( ), static_cast< DWORD >( buffer.size( ) ), &got ) )
            {
                broken = true;
                break;
            }
            if ( got == 0 )
                break;

            uint64_t take = got;
            if ( ranged && pos + take > hi + 1 )
                take = hi + 1 - pos;

            OVERLAPPED overlapped { };
            overlapped.Offset = static_cast< DWORD >( pos & 0xFFFFFFFFull );
            overlapped.OffsetHigh = static_cast< DWORD >( pos >> 32 );
            DWORD written = 0;
            if ( !WriteFile( file, buffer.data( ), static_cast< DWORD >( take ), &written, &overlapped ) || written != take )
            {
                fail( L"cannot write to the output file (disk full?)" );
                result = fetch_t::fatal;
                broken = true;
                break;
            }

            pos += take;
            g_downloaded += take;
            if ( ranged && pos > hi )
                break;
        }

        if ( result == fetch_t::fatal || g_abort || g_fatal )
            break;
        if ( broken )
            break;

        bool const complete = ranged ? pos > hi : ( known_total == 0 || pos == known_total );
        result = complete ? fetch_t::ok : fetch_t::retry;
    } while ( false );

    WinHttpCloseHandle( request );
    return result;
}

struct plan_t
{
    uint64_t total = 0;
    uint64_t chunk = 0;
    size_t count = 0;
    std::vector< char > done;
    std::mutex mutex;
    std::atomic< size_t > next { 0 };
    std::atomic< int > alive { 0 };
};

struct job_t
{
    HINTERNET session = nullptr;
    url_t url;
    std::wstring extra;
    std::wstring path;
    uint64_t total = 0;
    bool ranged = false;
};

HANDLE open_output( std::wstring const& path )
{
    return CreateFileW( path.c_str( ), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
}

void worker( job_t const& job, plan_t& plan )
{
    HANDLE const file = open_output( job.path );
    if ( file == INVALID_HANDLE_VALUE )
    {
        fail( L"cannot open the output file" );
        --plan.alive;
        return;
    }

    HINTERNET connect = WinHttpConnect( job.session, job.url.host.c_str( ), job.url.port, 0 );

    while ( !g_abort && !g_fatal )
    {
        size_t index = plan.next.fetch_add( 1 );
        for ( ;; )
        {
            if ( index >= plan.count )
                break;
            std::lock_guard< std::mutex > lock( plan.mutex );
            if ( !plan.done[ index ] )
                break;
            index = plan.next.fetch_add( 1 );
        }
        if ( index >= plan.count )
            break;

        uint64_t const lo = static_cast< uint64_t >( index ) * plan.chunk;
        uint64_t const hi = std::min( plan.total, lo + plan.chunk ) - 1;
        uint64_t pos = lo;

        int tries = 0;
        while ( pos <= hi && !g_abort && !g_fatal )
        {
            if ( !connect )
                connect = WinHttpConnect( job.session, job.url.host.c_str( ), job.url.port, 0 );

            fetch_t const r = connect ? fetch( connect, job.url, job.extra, pos, hi, job.total, file, true ) : fetch_t::retry;
            if ( r == fetch_t::ok )
                break;
            if ( r == fetch_t::fatal || g_abort || g_fatal )
                break;
            if ( ++tries > 12 )
            {
                fail( L"too many network errors" );
                break;
            }

            if ( connect )
                WinHttpCloseHandle( connect );
            connect = nullptr;
            Sleep( 300 * tries );
        }

        if ( pos > hi )
        {
            std::lock_guard< std::mutex > lock( plan.mutex );
            plan.done[ index ] = 1;
        }
    }

    if ( connect )
        WinHttpCloseHandle( connect );
    CloseHandle( file );
    --plan.alive;
}

// single connection: server without Range support or unknown size
void worker_single( job_t const& job, plan_t& plan )
{
    HANDLE const file = open_output( job.path );
    if ( file == INVALID_HANDLE_VALUE )
    {
        fail( L"cannot open the output file" );
        --plan.alive;
        return;
    }

    int tries = 0;
    while ( !g_abort && !g_fatal )
    {
        HINTERNET const connect = WinHttpConnect( job.session, job.url.host.c_str( ), job.url.port, 0 );
        uint64_t pos = 0;
        fetch_t const r = connect ? fetch( connect, job.url, job.extra, pos, 0, job.total, file, false ) : fetch_t::retry;
        if ( connect )
            WinHttpCloseHandle( connect );

        if ( r == fetch_t::ok )
        {
            LARGE_INTEGER end;
            end.QuadPart = static_cast< LONGLONG >( pos );
            SetFilePointerEx( file, end, nullptr, FILE_BEGIN );
            SetEndOfFile( file );
            std::lock_guard< std::mutex > lock( plan.mutex );
            plan.done.assign( 1, 1 );
            break;
        }
        if ( r == fetch_t::fatal || g_abort || g_fatal )
            break;

        g_downloaded -= pos; // start over: the stream cannot be resumed
        if ( ++tries > 8 )
        {
            fail( L"too many network errors" );
            break;
        }
        Sleep( 500 * tries );
    }

    CloseHandle( file );
    --plan.alive;
}

// ---------------------------------------------------------------- resume state
struct state_header_t
{
    char magic[ 4 ];
    uint64_t total;
    uint64_t chunk;
    uint64_t count;
};

void save_state( std::wstring const& path, plan_t& plan )
{
    HANDLE const file = CreateFileW( path.c_str( ), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
    if ( file == INVALID_HANDLE_VALUE )
        return;

    state_header_t header { { 'F', 'D', 'L', '1' }, plan.total, plan.chunk, plan.count };
    DWORD written = 0;
    WriteFile( file, &header, sizeof header, &written, nullptr );
    {
        std::lock_guard< std::mutex > lock( plan.mutex );
        WriteFile( file, plan.done.data( ), static_cast< DWORD >( plan.done.size( ) ), &written, nullptr );
    }
    CloseHandle( file );
}

bool load_state( std::wstring const& path, uint64_t total, plan_t& plan )
{
    HANDLE const file = CreateFileW( path.c_str( ), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
    if ( file == INVALID_HANDLE_VALUE )
        return false;

    state_header_t header { };
    DWORD read = 0;
    bool ok = ReadFile( file, &header, sizeof header, &read, nullptr ) && read == sizeof header && std::memcmp( header.magic, "FDL1", 4 ) == 0 && header.total == total && header.chunk > 0 &&
              header.count == ( total + header.chunk - 1 ) / header.chunk && header.count < ( 1ull << 26 );
    if ( ok )
    {
        plan.done.assign( static_cast< size_t >( header.count ), 0 );
        ok = ReadFile( file, plan.done.data( ), static_cast< DWORD >( header.count ), &read, nullptr ) && read == header.count;
        plan.chunk = header.chunk;
        plan.count = static_cast< size_t >( header.count );
    }
    CloseHandle( file );
    return ok;
}

bool file_exists( std::wstring const& path, uint64_t* size = nullptr )
{
    WIN32_FILE_ATTRIBUTE_DATA data { };
    if ( !GetFileAttributesExW( path.c_str( ), GetFileExInfoStandard, &data ) )
        return false;
    if ( size )
        *size = ( static_cast< uint64_t >( data.nFileSizeHigh ) << 32 ) | data.nFileSizeLow;
    return true;
}

std::wstring unique_name( std::wstring const& path )
{
    if ( !file_exists( path ) )
        return path;

    auto const dot = path.find_last_of( L'.' );
    auto const slash = path.find_last_of( L"\\/" );
    bool const has_ext = dot != std::wstring::npos && ( slash == std::wstring::npos || dot > slash );
    std::wstring const stem = has_ext ? path.substr( 0, dot ) : path;
    std::wstring const ext = has_ext ? path.substr( dot ) : L"";
    for ( int i = 1; i < 10000; ++i )
    {
        std::wstring candidate = stem + L" (" + std::to_wstring( i ) + L")" + ext;
        if ( !file_exists( candidate ) )
            return candidate;
    }
    return path;
}

// ---------------------------------------------------------------- ui
int console_width( )
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if ( GetConsoleScreenBufferInfo( GetStdHandle( STD_OUTPUT_HANDLE ), &info ) )
        return info.srWindow.Right - info.srWindow.Left + 1;
    return 100;
}

void print_usage( )
{
    out( L"fastdl " + std::wstring( k_version ) + L" - multi-connection downloader\n\n"
         L"usage: fastdl <url> [options]\n\n"
         L"  -o <file>      output file name (default: taken from the server / url)\n"
         L"  -t <n>         parallel connections, 1..128 (default 32)\n"
         L"  -c <mb>        chunk size in MB, 1..256 (default: automatic)\n"
         L"  -H \"N: v\"      extra request header (repeatable), e.g. -H \"Cookie: a=b\"\n"
         L"  -h             this help\n\n"
         L"Ctrl+C pauses the download; run the same command again to resume.\n" );
}

struct options_t
{
    std::wstring url;
    std::wstring output;
    int threads = 32;
    uint64_t chunk_mb = 0;
    std::wstring extra;
};

bool parse_args( int argc, wchar_t** argv, options_t& o )
{
    for ( int i = 1; i < argc; ++i )
    {
        std::wstring const a = argv[ i ];
        auto value = [ & ]( ) -> wchar_t const* { return i + 1 < argc ? argv[ ++i ] : nullptr; };

        if ( a == L"-h" || a == L"--help" || a == L"/?" )
            return false;
        else if ( a == L"-o" )
        {
            if ( auto const v = value( ) )
                o.output = v;
            else
                return false;
        }
        else if ( a == L"-t" )
        {
            auto const v = value( );
            if ( !v )
                return false;
            o.threads = std::max( 1, std::min( 128, _wtoi( v ) ) );
        }
        else if ( a == L"-c" )
        {
            auto const v = value( );
            if ( !v )
                return false;
            o.chunk_mb = static_cast< uint64_t >( std::max( 1, std::min( 256, _wtoi( v ) ) ) );
        }
        else if ( a == L"-H" )
        {
            auto const v = value( );
            if ( !v )
                return false;
            o.extra += std::wstring( v ) + L"\r\n";
        }
        else if ( o.url.empty( ) )
            o.url = a;
        else
            return false;
    }
    return !o.url.empty( );
}
} // namespace

int wmain( int argc, wchar_t** argv )
{
    HANDLE const console = GetStdHandle( STD_OUTPUT_HANDLE );
    DWORD mode = 0;
    g_console = GetConsoleMode( console, &mode ) != 0;
    _setmode( _fileno( stdout ), g_console ? _O_U16TEXT : _O_U8TEXT );
    if ( g_console )
        g_vt = SetConsoleMode( console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING ) != 0;
    SetConsoleCtrlHandler( on_ctrl, TRUE );

    options_t opt;
    if ( !parse_args( argc, argv, opt ) )
    {
        print_usage( );
        return 1;
    }

    url_t url;
    if ( !crack_url( opt.url, url ) )
    {
        out( L"error: not a valid http(s) url\n" );
        return 1;
    }

    HINTERNET const session = WinHttpOpen( ( L"fastdl/" + std::wstring( k_version ) ).c_str( ), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
    if ( !session )
    {
        out( L"error: WinHttpOpen failed\n" );
        return 1;
    }

    WinHttpSetTimeouts( session, 15000, 15000, 30000, 60000 );
    DWORD protocols = 0x800 | 0x2000; // TLS 1.2 | TLS 1.3
    if ( !WinHttpSetOption( session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols ) )
    {
        protocols = 0x800;
        WinHttpSetOption( session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols );
    }
    DWORD no_http2 = 0; // HTTP/2 would multiplex everything over one connection and defeat the point
    WinHttpSetOption( session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &no_http2, sizeof no_http2 );
    DWORD max_conns = static_cast< DWORD >( opt.threads + 4 );
    WinHttpSetOption( session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &max_conns, sizeof max_conns );

    out( L"Connecting to " + url.host + L" ...\n" );
    probe_t const info = probe( session, url, opt.extra );
    if ( !info.ok )
    {
        out( L"error: " + info.error + L"\n" );
        WinHttpCloseHandle( session );
        return 1;
    }

    url_t work = url;
    if ( !info.final_url.empty( ) )
    {
        url_t redirected;
        if ( crack_url( info.final_url, redirected ) )
            work = redirected;
    }

    std::wstring name = opt.output;
    if ( name.empty( ) )
        name = info.name.empty( ) ? name_from_url( work.path ) : info.name;
    if ( name.empty( ) && !info.final_url.empty( ) )
        name = name_from_url( work.path );
    name = sanitize( name );
    if ( name.empty( ) )
        name = L"download.bin";

    bool const ranged = info.ranges && info.total > 0;
    int const threads = ranged ? static_cast< int >( std::min< uint64_t >( opt.threads, std::max< uint64_t >( 1, info.total / ( 256 * 1024 ) ) ) ) : 1;

    out( L"File:    " + name + L"\n" );
    out( L"Size:    " + ( info.total ? fmt_size( static_cast< double >( info.total ) ) : std::wstring( L"unknown" ) ) + L"\n" );
    out( L"Mode:    " + ( ranged ? std::to_wstring( threads ) + L" connections (range requests)" : std::wstring( L"single connection (server does not support ranges)" ) ) + L"\n" );

    std::wstring const state_path = name + L".fdl";
    plan_t plan;
    plan.total = info.total;
    bool resumed = false;
    uint64_t existing = 0;
    bool const had_file = file_exists( name, &existing );

    if ( ranged && had_file && existing == info.total && load_state( state_path, info.total, plan ) )
        resumed = true;
    else if ( had_file && existing == info.total && !file_exists( state_path ) )
    {
        out( L"\n\"" + name + L"\" already exists with the expected size - nothing to do (delete it to download again).\n" );
        WinHttpCloseHandle( session );
        return 0;
    }
    else if ( had_file )
        name = unique_name( name );

    std::wstring const final_state = name + L".fdl";
    if ( !ranged )
    {
        plan.count = 1;
        plan.chunk = std::max< uint64_t >( info.total, 1 );
        plan.done.assign( 1, 0 );
    }
    else if ( !resumed )
    {
        uint64_t chunk = opt.chunk_mb ? opt.chunk_mb * 1024 * 1024 : std::max< uint64_t >( 1024 * 1024, std::min< uint64_t >( 16 * 1024 * 1024, info.total / ( static_cast< uint64_t >( threads ) * 8 ) ) );
        chunk = std::min( chunk, info.total );
        plan.chunk = chunk;
        plan.count = static_cast< size_t >( ( info.total + chunk - 1 ) / chunk );
        plan.done.assign( plan.count, 0 );
    }

    if ( !resumed )
    {
        HANDLE const file = CreateFileW( name.c_str( ), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( file == INVALID_HANDLE_VALUE )
        {
            out( L"error: cannot create \"" + name + L"\"\n" );
            WinHttpCloseHandle( session );
            return 1;
        }
        if ( ranged )
        {
            LARGE_INTEGER size;
            size.QuadPart = static_cast< LONGLONG >( info.total );
            if ( !SetFilePointerEx( file, size, nullptr, FILE_BEGIN ) || !SetEndOfFile( file ) )
            {
                CloseHandle( file );
                out( L"error: not enough disk space for the file\n" );
                WinHttpCloseHandle( session );
                return 1;
            }
        }
        CloseHandle( file );
    }
    else
    {
        uint64_t have = 0;
        for ( size_t i = 0; i < plan.count; ++i )
            if ( plan.done[ i ] )
                have += std::min( plan.total, ( i + 1 ) * plan.chunk ) - i * plan.chunk;
        g_downloaded = have;
        out( L"Resume:  " + fmt_size( static_cast< double >( have ) ) + L" already downloaded\n" );
    }
    out( L"\n" );

    job_t job;
    job.session = session;
    job.url = work;
    job.extra = opt.extra;
    job.path = name;
    job.total = info.total;
    job.ranged = ranged;

    uint64_t const start_bytes = g_downloaded;
    auto const t_start = clk::now( );
    plan.alive = threads;
    std::vector< std::thread > pool;
    for ( int i = 0; i < threads; ++i )
        pool.emplace_back( [ & ] { ranged ? worker( job, plan ) : worker_single( job, plan ); } );

    std::deque< std::pair< double, uint64_t > > samples;
    auto last_save = clk::now( );
    auto last_print = clk::now( );
    int const width = console_width( );
    size_t last_len = 0;

    auto render = [ & ]( bool final_line )
    {
        double const elapsed = std::chrono::duration< double >( clk::now( ) - t_start ).count( );
        uint64_t const now_bytes = g_downloaded.load( );
        samples.emplace_back( elapsed, now_bytes );
        while ( samples.size( ) > 2 && elapsed - samples.front( ).first > 3.0 )
            samples.pop_front( );

        double speed = 0;
        if ( samples.size( ) >= 2 && elapsed > samples.front( ).first )
            speed = static_cast< double >( now_bytes - samples.front( ).second ) / ( elapsed - samples.front( ).first );

        std::wstring line;
        if ( info.total )
        {
            double const fraction = std::min( 1.0, static_cast< double >( now_bytes ) / static_cast< double >( info.total ) );
            int const bar_width = std::max( 10, std::min( 40, width - 78 ) );
            int const filled = static_cast< int >( fraction * bar_width );
            std::wstring bar = L"[";
            if ( g_vt )
                bar += L"\x1b[32m";
            bar += std::wstring( filled, L'#' );
            if ( g_vt )
                bar += L"\x1b[90m";
            bar += std::wstring( bar_width - filled, L'-' );
            if ( g_vt )
                bar += L"\x1b[0m";
            bar += L"]";

            wchar_t percent[ 16 ];
            std::swprintf( percent, 16, L"%5.1f%%", fraction * 100.0 );
            double const remaining = static_cast< double >( info.total - std::min( info.total, now_bytes ) );
            line = bar + L" " + percent + L"  " + fmt_size( static_cast< double >( now_bytes ) ) + L" / " + fmt_size( static_cast< double >( info.total ) ) + L"  " + fmt_size( speed ) + L"/s  ETA " +
                   ( speed > 1 ? fmt_time( remaining / speed ) : std::wstring( L"--:--" ) ) + L"  " + std::to_wstring( plan.alive.load( ) ) + L" conn";
        }
        else
            line = fmt_size( static_cast< double >( now_bytes ) ) + L" downloaded  " + fmt_size( speed ) + L"/s  " + fmt_time( elapsed );

        size_t visible = 0;
        for ( size_t i = 0; i < line.size( ); ++i )
        {
            if ( line[ i ] == L'\x1b' )
                while ( i < line.size( ) && line[ i ] != L'm' )
                    ++i;
            else
                ++visible;
        }
        std::wstring pad( last_len > visible ? last_len - visible : 0, L' ' );
        if ( g_console )
            out( L"\r" + line + pad + ( final_line ? L"\n" : L"" ) );
        else
            out( line + L"\n" ); // redirected output: one plain line per update
        last_len = visible;
    };

    while ( plan.alive > 0 )
    {
        Sleep( 150 );
        if ( g_console || clk::now( ) - last_print > std::chrono::seconds( 2 ) )
        {
            render( false );
            last_print = clk::now( );
        }
        if ( ranged && clk::now( ) - last_save > std::chrono::seconds( 2 ) )
        {
            save_state( final_state, plan );
            last_save = clk::now( );
        }
    }
    for ( std::thread& t : pool )
        t.join( );
    render( true );

    bool all_done = true;
    for ( char c : plan.done )
        all_done = all_done && c;

    WinHttpCloseHandle( session );
    double const seconds = std::chrono::duration< double >( clk::now( ) - t_start ).count( );

    if ( g_abort )
    {
        if ( ranged )
            save_state( final_state, plan );
        out( L"\nPaused. Run the same command again to resume.\n" );
        return 130;
    }
    if ( g_fatal )
    {
        if ( ranged )
            save_state( final_state, plan );
        out( L"\nerror: " + g_fatal_msg + L"\n" );
        if ( ranged )
            out( L"Run the same command again to resume.\n" );
        return 1;
    }
    if ( !all_done || ( info.total && g_downloaded.load( ) < info.total ) )
    {
        if ( ranged )
            save_state( final_state, plan );
        out( L"\nerror: download incomplete\n" );
        return 1;
    }

    DeleteFileW( final_state.c_str( ) );
    uint64_t const got = g_downloaded.load( ) - start_bytes;
    out( L"\nDone: " + name + L" (" + fmt_size( static_cast< double >( g_downloaded.load( ) ) ) + L") in " + fmt_time( seconds ) + L", average " + fmt_size( seconds > 0 ? static_cast< double >( got ) / seconds : 0 ) + L"/s\n" );
    return 0;
}
