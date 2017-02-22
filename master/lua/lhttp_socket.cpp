#include <http_parser.h>
#include "lhttp_socket.h"
#include "ltools.h"
#include "../ev/ev_def.h"

int32 on_message_begin( http_parser *parser )
{
    UNUSED( parser );

    return 0;
}

int32 on_url( http_parser *parser, const char *at, size_t length )
{
    assert( "on_url empty parser",parser && (parser->data) );

    class lhttp_socket * http_socket = static_cast<class lhttp_socket *>(parser->data);
    http_socket->append_url( at,length );

    return 0;
}

int32 on_status( http_parser *parser, const char *at, size_t length )
{
    UNUSED( parser );
    UNUSED( at );
    UNUSED( length );
    // parser->status_code 本身有缓存，这里不再缓存
    return 0;
}

int32 on_header_field( http_parser *parser, const char *at, size_t length )
{
    assert( "on_header_field empty parser",parser && (parser->data) );

    class lhttp_socket * http_socket = static_cast<class lhttp_socket *>(parser->data);
    http_socket->append_cur_field( at,length );

    return 0;
}

int32 on_header_value( http_parser *parser, const char *at, size_t length )
{
    assert( "on_header_value empty parser",parser && (parser->data) );

    class lhttp_socket * http_socket = static_cast<class lhttp_socket *>(parser->data);
    http_socket->append_cur_value( at,length );

    return 0;
}

int32 on_headers_complete( http_parser *parser )
{
    UNUSED( parser );
    return 0;
}

int32 on_body( http_parser *parser, const char *at, size_t length )
{
    assert( "on_body empty parser",parser && (parser->data) );

    class lhttp_socket * http_socket = static_cast<class lhttp_socket *>(parser->data);
    http_socket->append_body( at,length );

    return 0;
}

int32 on_message_complete( http_parser *parser )
{
    assert( "on_message_complete empty parser",parser && (parser->data) );

    class lhttp_socket * http_socket = static_cast<class lhttp_socket *>(parser->data);
    http_socket->on_message_complete();

    return 0;
}

/* http chunk应该用不到，暂不处理 */
static const struct http_parser_settings settings = 
{
    on_message_begin,
    on_url,
    on_status,
    on_header_field,
    on_header_value,
    on_headers_complete,
    on_body,
    on_message_complete,

    NULL,NULL
};


lhttp_socket::lhttp_socket( lua_State *L )
    : lsocket( L )
{
    _parser   = NULL;
    _upgrade  = false;
    _cur_http = NULL;

    //HTTP_REQUEST, HTTP_RESPONSE, HTTP_BOTH
    _parser = new struct http_parser();
    http_parser_init( _parser,HTTP_BOTH );
    _parser->data = this;
}

lhttp_socket::~lhttp_socket()
{
    if ( _parser ) delete _parser;
    _parser = NULL;

    if ( _cur_http ) delete _cur_http;
    _cur_http = NULL;

    while ( !_http.empty() )
    {
        delete _http.front();
        _http.pop();
    }
}

const class lsocket *lhttp_socket::accept_new( int32 fd )
{
    /* 直接进入监听 */
    class lhttp_socket *_s = new class lhttp_socket( L );

    _s->socket::set<lsocket,&lsocket::message_cb>( _s );
    _s->socket::start( fd,EV_READ );  /* 这里会设置fd */

    return static_cast<class lsocket *>( _s );
}

int32 lhttp_socket::is_message_complete()
{
    uint32 dsize = _recv.data_size();
    assert( "http socket is_message_complete empty",dsize > 0 );

    if ( !_cur_http )
    {
        _cur_http = new http_info();
    }

    int32 nparsed = http_parser_execute( _parser,&settings,_recv.data(),dsize );

    _recv.clear(); // http_parser不需要旧缓冲区
    /* web_socket报文,暂时不用回调到上层.无论当前报文是否结束,返回false等待数据报文 */
    if ( _parser->upgrade )
    {
        _upgrade = true;
        return 0;
    }
    else if ( nparsed != (int32)dsize )  /* error */
    {
        int32 no = _parser->http_errno;
        ERROR( "http socket parse error(%d):%s",no,
            http_errno_name(static_cast<enum http_errno>(no)) );

        return -1;
    }

    return ( _http.empty() ? 0 : 1 );
}

void lhttp_socket::on_message_complete()
{
    assert( "no current http info",_cur_http );

    _cur_http->_method = _parser->method;
    _cur_http->_status_code = _parser->status_code;

    _http.push( _cur_http );

    _cur_field.clear();
    _old_field.clear();

    /* 需要重新申请一个http_info，因为web_socket可以同时发多个请求，而
     * http_parser_execute会一直解析直到完成
     */
    _cur_http = new http_info();
}

void lhttp_socket::append_url( const char *at,size_t len )
{
    assert( "no current http info",_cur_http );
    _cur_http->_url.append( at,len );
}

void lhttp_socket::append_body( const char *at,size_t len )
{
    assert( "no current http info",_cur_http );
    _cur_http->_body.append( at,len );
}

void lhttp_socket::append_cur_field( const char *at,size_t len )
{
    /* 解析到新字段，旧字段需要清除，否则value值错误 */
    if ( !_old_field.empty() ) _old_field.clear();

    _cur_field.append( at,len );
}

void lhttp_socket::append_cur_value( const char *at,size_t len )
{
    assert( "no current http info",_cur_http );

    /* 当前字段名已解析完，切换到旧字段 */
    if ( !_cur_field.empty() )
    {
        _old_field = _cur_field;
        _cur_field.clear();
    }

    std::string &val = _cur_http->_head_field[_old_field];
    val.append( at,len );
}

int32 lhttp_socket::get_head_field()
{
    if ( _http.empty() )
    {
        return 0;
    }

    const char *field = luaL_checkstring( L,1 );

    std::map<std::string,std::string> &head_field = _http.front()->_head_field;
    std::map<std::string,std::string>::iterator itr = head_field.find( field );
    if ( itr == head_field.end() )
    {
        return 0;
    }

    std::string &val = itr->second;
    lua_pushstring( L,val.c_str() );

    return 1;
}

int32 lhttp_socket::get_url()
{
    if ( _http.empty() )
    {
        return 0;
    }

    lua_pushstring( L,_http.front()->_url.c_str() );

    return 1;
}

int32 lhttp_socket::get_body()
{
    if ( _http.empty() )
    {
        return 0;
    }

    lua_pushstring( L,_http.front()->_body.c_str() );

    return 1;
}

int32 lhttp_socket::get_method()
{
    if ( _http.empty() )
    {
        return 0;
    }


    const char *method = http_method_str(
        static_cast<enum http_method>(_http.front()->_method) );
    lua_pushstring( L,method );

    return 1;
}

int32 lhttp_socket::get_status()
{
    if ( _http.empty() )
    {
        return 0;
    }

    int32 status_code = _http.front()->_status_code;
    lua_pushinteger( L,status_code );

    return 1;
}

int32 lhttp_socket::next()
{
    int32 peek = lua_toboolean( L,1 ); /* socket MSG_PEEK */

    if ( _http.empty() )
    {
        lua_pushboolean( L,0 );
        return 1;
    }

    if ( !peek )
    {
        delete _http.front();
        _http.pop();
    }

    lua_pushboolean( L,_http.size() );

    return 1;
}

int32 lhttp_socket::is_upgrade()
{
    lua_pushboolean( L,_upgrade );

    return 1;
}
