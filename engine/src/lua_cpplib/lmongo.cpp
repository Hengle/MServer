#include <cstdarg>
#include <chrono>

#include <lbson.h>

#include "ltools.hpp"
#include "lmongo.hpp"
#include "../system/static_global.hpp"

#define MONGODB_EVENT "mongodb_event"

LMongo::LMongo(lua_State *L)
    : Thread("lmongo"), _query_pool("lmongo"), _result_pool("lmongo")
{
    _dbid = luaL_checkinteger(L, 2);
}

LMongo::~LMongo()
{
    if (!_query.empty())
    {
        ERROR("mongo query not clean, abort");
        while (!_query.empty())
        {
            _query_pool.destroy(_query.front());
            _query.pop();
        }
    }

    if (!_result.empty())
    {
        ERROR("mongo result not clean, abort");
        while (!_result.empty())
        {
            _result_pool.destroy(_result.front());
            _result.pop();
        }
    }
}

// 连接数据库
// 由于是开启线程去连接，并且是异步的，需要上层定时调用valid来检测是否连接上
int32_t LMongo::start(lua_State *L)
{
    if (active())
    {
        return luaL_error(L, "mongo thread already active");
    }

    const char *ip     = luaL_checkstring(L, 1);
    const int32_t port = luaL_checkinteger(L, 2);
    const char *usr    = luaL_checkstring(L, 3);
    const char *pwd    = luaL_checkstring(L, 4);
    const char *db     = luaL_checkstring(L, 5);

    set(ip, port, usr, pwd, db);
    Thread::start(5000000);

    return 0;
}

int32_t LMongo::stop(lua_State *L)
{
    UNUSED(L);
    Thread::stop();

    return 0;
}

bool LMongo::initialize()
{
    if (connect())
    {
        ERROR("mongo connect fail");
        return false;
    }

    // 不断地重试，直到连上重试
    int32_t ok = 0;
    do
    {
        ok = ping();

        // 连接成功
        if (0 == ok) break;

        // 连接出错，直接退出
        if (ok > 0)
        {
            disconnect();
            return false;
        }

        // 连接进行中，继续阻塞等待
        std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (active());

    if (0 == ok) wakeup_main(S_READY);

    return true;
}

size_t LMongo::busy_job(size_t *finished, size_t *unfinished)
{
    lock();
    size_t finished_sz   = _result.size();
    size_t unfinished_sz = _query.size();

    if (is_busy()) unfinished_sz += 1;
    unlock();

    if (finished) *finished = finished_sz;
    if (unfinished) *unfinished = unfinished_sz;

    return finished_sz + unfinished_sz;
}

void LMongo::routine(int32_t ev)
{
    UNUSED(ev);
    /* 如果某段时间连不上，只能由下次超时后触发
     * 超时时间由thread::start参数设定
     */
    if (ping()) return;

    lock();
    while (!_query.empty())
    {
        MongoQuery *query = _query.front();
        _query.pop();

        MongoResult *res = _result_pool.construct(query->_qid, query->_mqt);

        unlock();
        bool ok = do_command(query, res);
        lock();

        _query_pool.destroy(query);
        if (ok)
        {
            _result.push(res);
            wakeup_main(S_DATA);
        }
        else
        {
            _result_pool.destroy(res);
        }
    }
    unlock();
}

bool LMongo::uninitialize()
{
    disconnect();

    return true;
}

void LMongo::on_ready(lua_State *L)
{
    LUA_PUSHTRACEBACK(L);
    lua_getglobal(L, MONGODB_EVENT);
    lua_pushinteger(L, S_READY);
    lua_pushinteger(L, _dbid);

    if (LUA_OK != lua_pcall(L, 2, 0, 1))
    {
        ERROR("mongodb on ready error:%s", lua_tostring(L, -1));
        lua_pop(L, 1); /* remove error message */
    }

    lua_pop(L, 1);
}

void LMongo::main_routine(int32_t ev)
{
    static lua_State *L = StaticGlobal::state();

    if (EXPECT_FALSE(ev & S_READY)) on_ready(L);

    LUA_PUSHTRACEBACK(L);

    lock();
    while (!_result.empty())
    {
        MongoResult *res = _result.front();
        _result.pop();

        unlock();
        on_result(L, res);
        lock();

        _result_pool.destroy(res);
    }
    unlock();

    lua_pop(L, 1); /* remove stacktrace */
}

void LMongo::on_result(lua_State *L, const MongoResult *res)
{
    // 为0表示不需要回调到脚本
    if (0 == res->_qid) return;

    lua_getglobal(L, MONGODB_EVENT);

    lua_pushinteger(L, S_DATA);
    lua_pushinteger(L, _dbid);
    lua_pushinteger(L, res->_qid);
    lua_pushinteger(L, res->_error.code);

    int32_t nargs = 4;
    if (res->_data)
    {
        struct error_collector error;
        bson_type_t root_type =
            res->_mqt == MQT_FIND ? BSON_TYPE_ARRAY : BSON_TYPE_DOCUMENT;

        if (lbs_do_decode(L, res->_data, root_type, &error) < 0)
        {
            lua_pop(L, 4);
            ERROR("mongo result decode error:%s", error.what);

            // 即使出错，也回调到脚本
        }
        else
        {
            nargs++;
        }
    }

    if (LUA_OK != lua_pcall(L, nargs, 0, 1))
    {
        ERROR("mongo call back error:%s", lua_tostring(L, -1));
        lua_pop(L, 1); /* remove error message */
    }
}

/* 在子线程触发查询命令 */
bool LMongo::do_command(const MongoQuery *query, MongoResult *res)
{
    bool ok          = false;
    auto begin       = std::chrono::steady_clock::now();
    switch (query->_mqt)
    {
    case MQT_COUNT: ok = Mongo::count(query, res); break;
    case MQT_FIND: ok = Mongo::find(query, res); break;
    case MQT_FMOD: ok = Mongo::find_and_modify(query, res); break;
    case MQT_INSERT: ok = Mongo::insert(query, res); break;
    case MQT_UPDATE: ok = Mongo::update(query, res); break;
    case MQT_REMOVE: ok = Mongo::remove(query, res); break;
    default:
    {
        ERROR("unknow handle mongo command type:%d\n", query->_mqt);
        return false;
    }
    }

    assert((ok && 0 == res->_error.code) || (!ok && 0 != res->_error.code));

    auto end = std::chrono::steady_clock::now();
    res->_elaspe =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

    return true;
}

bson_t *LMongo::string_or_table_to_bson(lua_State *L, int index, int opt,
                                        bson_t *bs, ...)
{
#define CLEAN_BSON(arg)                                                          \
    do                                                                           \
    {                                                                            \
        va_list args;                                                            \
        for (va_start(args, arg); arg != END_BSON; arg = va_arg(args, bson_t *)) \
        {                                                                        \
            if (arg) bson_destroy(arg);                                          \
        }                                                                        \
        va_end(args);                                                            \
    } while (0)

    bson_t *bson = nullptr;
    if (lua_istable(L, index)) // 自动将lua table 转化为bson
    {
        struct error_collector error;
        if (!(bson = lbs_do_encode(L, index, nullptr, &error)))
        {
            CLEAN_BSON(bs);
            luaL_error(L, "table to bson error:%s", error.what);
            return nullptr;
        }

        return bson;
    }
    else if (lua_isstring(L, index)) // json字符串
    {
        const char *json = lua_tostring(L, index);
        bson_error_t error;
        bson = bson_new_from_json(reinterpret_cast<const uint8_t *>(json), -1,
                                  &error);
        if (!bson)
        {
            CLEAN_BSON(bs);
            luaL_error(L, "json to bson error:%s", error.message);
            return nullptr;
        }

        return bson;
    }

    if (0 == opt) return nullptr;
    if (1 == opt) return bson_new();

    luaL_error(L, "argument #%d expect table or json string", index);
    return nullptr;

#undef CLEAN_BSON
}

int32_t LMongo::count(lua_State *L)
{
    if (!active())
    {
        return luaL_error(L, "mongo thread not active");
    }

    int32_t id             = luaL_checkinteger(L, 1);
    const char *collection = luaL_checkstring(L, 2);
    if (!collection)
    {
        return luaL_error(L, "mongo count:collection not specify");
    }

    bson_t *query  = string_or_table_to_bson(L, 3, 0);
    bson_t *opts   = string_or_table_to_bson(L, 4, 0, query, END_BSON);

    lock();
    MongoQuery *mongo_count =
        _query_pool.construct(id, MQT_COUNT, collection, query, opts);

    _query.push(mongo_count);
    wakeup(S_DATA);
    unlock();

    return 0;
}

int32_t LMongo::find(lua_State *L)
{
    if (!active())
    {
        return luaL_error(L, "mongo thread not active");
    }

    int32_t id             = luaL_checkinteger(L, 1);
    const char *collection = luaL_checkstring(L, 2);
    if (!collection)
    {
        return luaL_error(L, "mongo find:collection not specify");
    }

    bson_t *query = string_or_table_to_bson(L, 3, 1);
    bson_t *opts  = string_or_table_to_bson(L, 4, 0, query, END_BSON);

    lock();
    MongoQuery *mongo_find =
        _query_pool.construct(id, MQT_FIND, collection, query, opts);

    _query.push(mongo_find);
    wakeup(S_DATA);
    unlock();

    return 0;
}

int32_t LMongo::find_and_modify(lua_State *L)
{
    if (!active())
    {
        return luaL_error(L, "mongo thread not active");
    }

    int32_t id             = luaL_checkinteger(L, 1);
    const char *collection = luaL_checkstring(L, 2);
    if (!collection)
    {
        return luaL_error(L, "mongo find_and_modify:collection not specify");
    }

    bson_t *query  = string_or_table_to_bson(L, 3, 1);
    bson_t *sort   = string_or_table_to_bson(L, 4, 0, query, END_BSON);
    bson_t *update = string_or_table_to_bson(L, 5, 1, query, sort, END_BSON);
    bson_t *fields =
        string_or_table_to_bson(L, 6, 0, query, sort, update, END_BSON);

    bool remove  = lua_toboolean(L, 7);
    bool upsert  = lua_toboolean(L, 8);
    bool ret_new = lua_toboolean(L, 9);

    lock();
    MongoQuery *mongo_fmod =
        _query_pool.construct(id, MQT_FMOD, collection, query);

    mongo_fmod->_sort   = sort;
    mongo_fmod->_update = update;
    mongo_fmod->_fields = fields;

    mongo_fmod->_remove = remove;
    mongo_fmod->_upsert = upsert;
    mongo_fmod->_new    = ret_new;

    _query.push(mongo_fmod);
    wakeup(S_DATA);
    unlock();

    return 0;
}

/* insert( id,collections,info ) */
int32_t LMongo::insert(lua_State *L)
{
    if (!active())
    {
        return luaL_error(L, "mongo thread not active");
    }

    int32_t id             = luaL_checkinteger(L, 1);
    const char *collection = luaL_checkstring(L, 2);
    if (!collection)
    {
        return luaL_error(L, "mongo insert:collection not specify");
    }

    bson_t *query = string_or_table_to_bson(L, 3);

    lock();
    MongoQuery *mongo_insert =
        _query_pool.construct(id, MQT_INSERT, collection, query);

    _query.push(mongo_insert);
    wakeup(S_DATA);
    unlock();

    return 0;
}

int32_t LMongo::update(lua_State *L)
{
    if (!active())
    {
        return luaL_error(L, "mongo thread not active");
    }

    int32_t id             = luaL_checkinteger(L, 1);
    const char *collection = luaL_checkstring(L, 2);
    if (!collection)
    {
        return luaL_error(L, "mongo update:collection not specify");
    }

    bson_t *query  = string_or_table_to_bson(L, 3);
    bson_t *update = string_or_table_to_bson(L, 4, -1, query, END_BSON);

    int32_t upsert = lua_toboolean(L, 5);
    int32_t multi  = lua_toboolean(L, 6);

    lock();
    MongoQuery *mongo_update =
        _query_pool.construct(id, MQT_UPDATE, collection, query);
    mongo_update->_update = update;
    mongo_update->_flags =
        (upsert ? MONGOC_UPDATE_UPSERT : MONGOC_UPDATE_NONE)
        | (multi ? MONGOC_UPDATE_MULTI_UPDATE : MONGOC_UPDATE_NONE);

    _query.push(mongo_update);
    wakeup(S_DATA);
    unlock();

    return 0;
}

int32_t LMongo::remove(lua_State *L)
{
    if (!active())
    {
        return luaL_error(L, "mongo thread not active");
    }

    int32_t id             = luaL_checkinteger(L, 1);
    const char *collection = luaL_checkstring(L, 2);
    if (!collection)
    {
        return luaL_error(L, "mongo remove:collection not specify");
    }

    bson_t *query = string_or_table_to_bson(L, 3);

    int32_t single = lua_toboolean(L, 4);

    lock();
    MongoQuery *mongo_remove =
        _query_pool.construct(id, MQT_REMOVE, collection, query);
    mongo_remove->_flags =
        single ? MONGOC_REMOVE_SINGLE_REMOVE : MONGOC_REMOVE_NONE;

    _query.push(mongo_remove);
    wakeup(S_DATA);
    unlock();

    return 0;
}
