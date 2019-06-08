#ifndef __BUFFER_H__
#define __BUFFER_H__

#include "../global/global.h"
#include "../pool/ordered_pool.h"

/*
 *    +---------------------------------------------------------------+
 *    |    悬空区   |        数据区          |      空白buff区          |
 *    +---------------------------------------------------------------+
 * _buff          _pos                    _size
 *
 *收发缓冲区，要考虑游戏场景中的几个特殊情况：
 * 1.游戏的通信包一般都很小，reserved出现的概率很小。偶尔出现，memcpy的效率也是可以接受的
 * 2.缓冲区可能出现边读取边接收，边发送边写入的情况。因此，当前面的协议未读取完，又接收到新的
 *   协议，或者前面的数据未发送完，又写入新的数据，会造成前面一段缓冲区悬空，没法利用。旧框架
 *   的办法是每读取完或发送完一轮数据包，就用memmove把后面的数据往前面移动，这在粘包频繁出现
 *   并且包不完整的情况下效率很低(进程间的socket粘包很严重)。因此，我们忽略前面的悬空缓冲区，
 *   直到我们需要调整内存时，才用memmove移动内存。
 */

/* 
class buffer
{
public:
    buffer();
    ~buffer();

    static void purge() { allocator.purge(); }

    bool append( const void *data,uint32 len )
        __attribute__ ((warn_unused_result))
    {
        if ( !reserved( len ) ) return false;

        __append( data,len );    return true;
    }

    // 减去缓冲区数据，此函数不处理缓冲区的数据
    inline void subtract( uint32 len )
    {
        _pos += len;
        assert( "buffer subtract",_size >= _pos && _len >= _pos );

        if ( _size == _pos ) _pos = _size = 0;
    }

    // 增加数据区
    inline void increase( uint32 len )
    {
        _size += len;
        assert( "buffer increase",_size <= _len );
    }

    // 重置
    inline void clear() { _pos = _size = 0; }
    // 总大小
    inline uint32 length() const { return _len; }

    // 数据区大小
    inline uint32 data_size() const { return _size - _pos; }
    // 数据区指针 
    inline char *data_pointer() const { return _buff + _pos; }

    // 缓冲区大小
    inline uint32 buff_size() const { return _len - _size; }
    // 缓冲区指针
    inline char *buff_pointer() const { return _buff + _size; }

    // raw append data,but won't reserved
    void __append( const void *data,const uint32 len )
    {
        assert( "buffer not reserved!",_len - _size >= len );
        memcpy( _buff + _size,data,len );       _size += len;
    }

    // 设置缓冲区最大最小值
    void set_buffer_size( uint32 max,uint32 min )
    {
        _max_buff = max;
        _min_buff = min;
    }
public:
    /* 内存预分配：
     * @bytes : 要增长的字节数。默认为0,首次分配BUFFER_CHUNK，用完再按指数增长
     * @vsz   : 已往缓冲区中写入的字节数，但未增加_size偏移，主要用于自定义写入缓存
     /
    inline bool reserved( uint32 bytes = 0,uint32 vsz = 0 )
        __attribute__ ((warn_unused_result))
    {
        uint32 size = _size + vsz;
        if ( _len - size > bytes ) return true;/* 不能等于0,刚好用完也申请 /

        if ( _pos )    /* 解决悬空区 /
        {
            assert( "reserved memmove error",_size > _pos && size <= _len );
            memmove( _buff,_buff + _pos,size - _pos );
            _size -= _pos;
            _pos   = 0   ;

            return reserved( bytes,vsz );
        }

        assert( "buffer no min or max setting",_min_buff > 0 && _max_buff > 0 );

        uint32 new_len = _len  ? _len  : _min_buff;
        uint32 _bytes  = bytes ? bytes : BUFFER_CHUNK;
        while ( new_len - size < _bytes )
        {
            new_len *= 2;  /* 通用算法：指数增加 /
        }

        if ( new_len > _max_buff ) return false;

        /* 检验内在分配大小是否符合机制 /
        assert( "buffer chunk size error",0 == new_len%BUFFER_CHUNK );

        uint32 chunk_size = new_len >= BUFFER_LARGE ? 1 : BUFFER_CHUNK_SIZE;
        char *new_buff =
            allocator.ordered_malloc( new_len/BUFFER_CHUNK,chunk_size );

        /* 像STL一样把旧内存拷到新内存 /
        if ( size ) memcpy( new_buff,_buff,size );

        if ( _len ) allocator.ordered_free( _buff,_len/BUFFER_CHUNK );

        _buff = new_buff;
        _len  = new_len ;

        return      true;
    }
private:
    buffer( const buffer & );
    buffer &operator=( const buffer &);
private:
    char  *_buff;    /* 缓冲区指针 /
    uint32 _size;    /* 缓冲区已使用大小 /
    uint32 _len ;    /* 缓冲区总大小 /
    uint32 _pos ;    /* 悬空区大小 /

    uint32 _max_buff; /* 缓冲区最小值 /
    uint32 _min_buff; /* 缓冲区最大值 /
private:
    static class ordered_pool<BUFFER_CHUNK> allocator;
};
*/


/* 网络收发缓冲区
 * 
 * 1. 连续缓冲区
 *    整个缓冲区只用一块内存，不够了按2倍重新分配，把旧数据拷贝到新缓冲区。分配了就不再释放
 *    1). 缓冲区是连续的，存取效率高。socket读写可以直接用缓冲区，不需要二次拷贝,
 *        不需要二次读写。websocket、bson等打包数据时，可以直接用缓冲区，无需二次拷贝.
 *    2). 发生迁移拷贝时效率不高，缓冲区在极限情况下可达1G。
 *        比如断点调试，线上服务器出现过因为数据库阻塞用了1G多内存的
 *    3). 分配了不释放，内存利用率不高。当链接很多时，内存占用很高
 * 2. 小包链表设计
 *    每个包一个小块，用链表管理
 *    1). 不用考虑合并包
 *    2). 当包长无法预估时，一样需要重新分配，发生拷贝
 *        有些项目直接全部分配最大包，超过包大小则由上层逻辑分包，但这样包利用率较低
 *    3). 收包时需要额外处理包不在同连续内存块的情况，protobuf这些要求同一个数据包的buff是
 *        连续的才能解析
 *    4). socket读写时需要多次读写。因为可读、可写的大小不确定，而包比较小，可能读写不完
 * 3. 中包链表设计
 *    预分配一个比较大的包(比如服务器连接为8M)，数据依次添加到这个包。
 *    正常情况下，这个数据包无需扩展。超过这个包大小，会继续分配更多的包到链表上
 *   1). 数组包基本是连续的，利用率较高
 *   2). 包可以预分配比较大(超过协议最大长度)，很多情况下是可以当作连续缓冲区用的
 *   3). socket读写时不需要多次读写。因为包已经很大了，一次读写不太可能超过包大小
 *       epoll为ET模式也需要多次读写，但这里现在用LT
 *   4). 需要处理超过包大小时，合并、拆分的情况
 */

// TODO: static char continuous_ctx[MAX_PACKET_LEN] = { 0 }; 放C++文件

class buffer
{
public:
    buffer()
    {
        _front = _back = new_chunk();
    }

    ~buffer()
    {
        while (_front)
        {
            chunk_t *tmp = _front;
            _front = _front->_next;

            del_chunk( tmp );
        }

        _front = _back = NULL;
    }

    // 添加数据
    void append( const void *data,const uint32 len )
    {
        uint32 append_sz = 0;
        do
        {
            reserved();
            uint32 space = _back->space_size();

            uint32 size = MATH_MIN( space,len - append_sz );
            _back->append( data + append_sz,size );

            append_sz += size;

            // 大多数情况下，一次应该可以添加完数据
            // 如果不能，考虑调整单个chunk的大小，否则影响效率
        }while ( expect_false(append_sz < len) );
    }

    // 删除数据
    void remove( uint32 len )
    {
        uint32 remove_sz = 0;
        do
        {
            uint32 used = _front->used_size();

            // 这个chunk还有其他数据
            if ( used > len )
            {
                _front->remove( len );
                break;
            }

            // 这个chunk只剩下这个数据包
            if ( used == len )
            {
                chunk_t *next = _front->_next;
                if ( next )
                {
                    // 还有下一个chunk，则指向下一个chunk
                    del_chunk( _front );
                    _front = next;
                }
                else
                {
                    _front->clear(); // 在无数据的时候重重置缓冲区
                }
                break;
            }

            // 这个数据包分布在多个chunk，一个个删
            chunk_t *tmp = _front;

            _front = _front->_next;
            assert( "no more chunk to remove",_front );

            del_chunk( tmp );
            remove_sz += used;
        } while( true );
    }

    // 只获取第一个chunk的有效数据大小，用于socket发送
    inline uint32 get_used_size() const { return _front->used_size(); }
    // 只获取第一个chunk的有效数据指针，用于socket发送
    inline char *get_used_ctx() const { return _front->used_ctx(); };

    /* 检测当前有效数据的大小是否 >= 指定值
     * TODO:用于数据包分在不同chunk的情况，这是采用这种设计缺点之一
     */
    inline bool check_used_size( uint32 len ) const
    {
        uint32 used = 0;
        const chunk_t *next = _front;

        do
        {
            used += next->used_size();

            next = next->_next;
        } while ( expect_false(next && used < len) )

        return used >= len;
    }

    /* 检测指定长度的有效数据是否在连续内存，不在的话要合并成连续内存
     * protobuf这些都要求内存在连续缓冲区才能解析
     * TODO:这是采用这种设计缺点之二
     */
    inline char *check_used_ctx( uint32 len )
    {
        // 大多数情况下，是在同一个chunk的，如果不是，调整下chunk的大小，否则影响效率
        if ( expect_true( _front->used_size() >= len ) )
        {
            return _front->used_ctx();
        }

        static char ctx[MAX_PACKET_LEN] = { 0 };

        uint32 used = 0;
        const chunk_t *next = _front;

        do
        {
            uint32 next_used = next->used_size();
            memcpy( continuous_ctx + used,
                next->used_ctx(),MATH_MIN( len - used,next_used ) );

            used += next_used;
            next = next->_next;
        } while ( next && used < len )

        assert( "check_used_ctx fail", used == len );
        return continuous_ctx;
    }

    inline char *check_all_used_ctx( uint32 &len )
    {
        if ( expect_true( !_front->_next ) )
        {
            len = _front->used_size();
            return _front->used_ctx();
        }

        uint32 used = 0;
        const chunk_t *next = _front;

        do
        {
            uint32 next_used = next->used_size();
            memcpy( continuous_ctx + used,next->used_ctx(),next_used );

            used += next_used;
            next = next->_next;
        } while ( next )

        len = used;
        assert( "check_used_ctx fail", used > 0 );
        return continuous_ctx;
    }

    // 获取空闲缓冲区指针及大小，只获取一个chunk的，用于socket接收
    char *get_space_ctx(uint32 &space);
    // 增加有效数据长度，比如从socket读数据时，先拿缓冲区，然后才知道读了多少数据
    inline void add_used_offset(uint32 len)
    {
        _back->add_used_offset( len );
    }

    /* 预分配一块连续的空闲缓冲区，大小不能超过单个chunk
     * @len:len为0表示不需要确定预分配
     * 注意当len不为0时而当前chunk空间不足，会直接申请下一个chunk，数据包并不是连续的
     */
    void reserved(uint32 len = 0)
    {
        uint32 space = _back->space_size();
        if ( 0 == space || len > space )
        {
            _back = _back->_next = new_chunk( len );
        }
    }

private:
    inline chunk_t *new_chunk( uint32 ctx_size  = 0 )
    {
        chunk_t *chunk = new chunk_t();
        memset(chunk,0,sizeof(chunk_t));

        chunk->_ctx = new_ctx( ctx_size );
        chunk->_max = ctx_size;

        return chunk;
    }

    inline del_chunk( chunk_t *chunk )
    {

    }

    // @size:要分配的缓冲区大小，会被修正为最终分配的大小
    inline char *new_ctx( uint32 &size )
    {

    }

    inline del_ctx( char *ctx,uint32 size )
    {
    }
private:
    typedef struct
    {
        char  *_ctx;    /* 缓冲区指针 */
        uint32 _max;    /* 缓冲区总大小 */

        uint32 _beg;    /* 有效数据开始位置 */
        uint32 _end;    /* 有效数据结束位置 */

        chunk_t *_next; /* 链表下一节点 */

        inline remove( uint32 len )
        {
            _beg += len;
            assert("chunk remove corruption",_end >= _beg );
        }
        inline void add_used_offset( uint32 len )
        {
            _end += len;
            assert("chunk append corruption",_max >= _end );
        }
        inline append( const void *data,const uint32 len )
        {
            add_used_offset( len );
            memcpy( _ctx,data,len );
        }

        inline char *used_ctx() { return _ctx + _beg; } // 有效数据指针
        inline char *space_ctx() { return _ctx + _end; } // 空闲缓冲区指针

        inline void clear() { _beg = _end = 0; } // 重置有效数据
        inline uint32 used_size() const { return _end - _beg; } // 有效数据大小
        inline uint32 space_size() const { return _max - _end; } // 空闲缓冲区大小
    }chunk_t;

    chunk_t *_front; // 数据包链表头
    chunk_t *_back ; // 数据包链表尾
    uint32 _chunk_size; // 已申请chunk数量

    uint32 _chunk_max; // 允许申请chunk的最大数量
    uint32 _chunk_ctx_max; // 单个chunk的缓冲区大小
};

#endif /* __BUFFER_H__ */
