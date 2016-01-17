#include "../redis_cluster.hpp"
#include <unistd.h>
#include <stdio.h>
#include <ncurses.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <iostream>
#include <map>
#include <hiredis/hiredis.h>


#ifdef SUPPORT_RRD
#include "rrd.h"
static char rrd_file[]= "./infinit.rrd";
#endif

typedef struct {
    uint64_t read;       // set by read thread
    uint64_t read_t;     // set by read thread
    uint64_t read_ttl;   // set by read thread
    uint64_t read_error; // set by read thread
    uint64_t read_lost;  // set by read thread
    uint64_t unmatch;    // set by read thread

    uint64_t write;      // set by write thread
    uint64_t write_new;  // set by write thread
    uint64_t write_t;    // set by write thread
    uint64_t write_ttl;  // set by write thread
    uint64_t write_error;// set by write thread
} stat_item_t;

static const stat_item_t stat_item_initial = {0,0,0,0,0,0,0,0,0};
#define DUMP_STAT_ITEM(it) " r: "<< (it).read << " w: " << (it).write << \
                           " rt: " << (it).read_t << " wt: " << (it).write_t << \
                           " re: " << (it).read_error << " we: " << (it).write_error << \
                           " um: " << (it).unmatch << " "

typedef struct {
    int now_sec;
    int now_us;
    int last_us;
} time_period_t;

typedef struct {
    bool         is_running; // set by main thread , read by work thread
    pthread_t    tid_w;      // thread id of thread_write
    pthread_t    tid_r;      // thread id of thread_read
    unsigned int key_seed;   // set by thread_write, read by thread_read, for reproducing keys in its read thread
    unsigned int seed_count; // set by thread_write, read by thread_read, number of rand_r() calls, for reproducing keys in its read thread
    stat_item_t  stat;       // set by work thread , read by main thread
} work_thread_data_t;

#define THREAD_INITIAL_KEY_SEED 0

typedef std::map<std::string, std::string> CacheType;

volatile int  conf_load_of_thread = 6;
#define MAX_LOAD_OF_THREAD 10

volatile int  conf_threads_num = 4;
#define MAX_THREADS_NUM 20

volatile bool conf_is_running = true;

redis::cluster::Cluster *cluster_ = NULL;
CacheType          cache;
pthread_spinlock_t c_lock; //cache lock

std::string get_random_key(unsigned int &seed, unsigned int &count) {
    int n = 10000*(rand_r(&seed)/(RAND_MAX+0.1));
    std::ostringstream ss;
    ss << "key_" << n;
    count++;
    return ss.str();
}

std::string get_random_value(unsigned int &seed) {
    int n = 100000*(rand_r(&seed)/(RAND_MAX+0.1));
    std::ostringstream ss;
    ss << "value_" << n;
    return ss.str();
}

int check_point(int &now_us, int &now_sec, int &last_us) {
    struct timeval tv;
    gettimeofday( &tv, NULL );
    now_sec = tv.tv_sec;
    now_us = tv.tv_sec*1000000 + tv.tv_usec;

    int ret = now_us - last_us;
    last_us = now_us;
    return ret;
}

int redis_set(const std::string &key, const std::string& value) {
    int ret = 0;
    std::vector<std::string> commands;
    commands.push_back("SET");
    commands.push_back(key);
    commands.push_back(value);
    redisReply *reply = cluster_->run(commands);
    if( !reply ) {
        ret = -1;
    } else if( reply->type==REDIS_REPLY_STATUS && !strcmp(reply->str, "OK") ) {
        ret = 0;
    } else if( reply->type==REDIS_REPLY_ERROR ) {
        //std::cout << "redis_set error " << reply->str << std::endl;
        ret = -1;
    } else {
        ret = -1;
    }

    if( reply )
        freeReplyObject( reply );

    return ret;
}

int redis_get(const std::string &key, std::string& value) {
    int ret = 0;
    std::vector<std::string> commands;
    commands.push_back("GET");
    commands.push_back(key);
    redisReply *reply = cluster_->run(commands);
    if( !reply ) {
        ret = -1;
    } else if( reply->type==REDIS_REPLY_NIL ) {
        ret = 1; //not found
    } else if( reply->type==REDIS_REPLY_STRING ) {
        value = reply->str;
        ret = 0;
    } else if( reply->type==REDIS_REPLY_ERROR ) {
        //std::cout << "redis_get error " << reply->str << std::endl;
        ret = -1;
    } else {
        ret = -1;
    }

    if( reply )
        freeReplyObject( reply );

    return ret;
}

void* thread_write(void* para) {
    int now_us = 0;
    int now_sec = 0;
    int last_us = 0;
    unsigned int key_seed;
    unsigned int value_seed;
    unsigned int seed_count;
    volatile work_thread_data_t *pmydata = (work_thread_data_t *)para;
    volatile stat_item_t        *pstat   = &pmydata->stat;

    check_point(now_us, now_sec, last_us);
    value_seed = now_us;
    key_seed   = now_us;
    pmydata->key_seed = key_seed;
    seed_count = pmydata->seed_count;
    while( pmydata->is_running) {

        std::string key = get_random_key(key_seed, seed_count);
        std::string value_read;
        std::string value_write;

        /* check  */
        {
            redis::cluster::LockGuard lockguard(c_lock);
            CacheType::iterator iter = cache.find( key );
            if( iter != cache.end() )  {
                value_write = iter->second;
            }
        }

        if(value_write.length() == 0) {
            int rv = redis_get(key, value_read);
            if(rv == 0) {
                value_write = value_read;
                redis::cluster::LockGuard lockguard(c_lock);
                cache[key] = value_write;
            } else {
                redis::cluster::LockGuard lockguard(c_lock);
                CacheType::iterator iter = cache.find( key );
                if( iter != cache.end() )  {
                    value_write = iter->second;
                } else {
                    value_write =  get_random_value(value_seed);
                    pstat->write_new++;
                    cache[key] = value_write;
                }
            }
        }

        /* set */
        check_point(now_us, now_sec, last_us);
        int rv = redis_set(key, value_write);
        pstat->write_ttl += cluster_->ttls();
        pstat->write ++;
        if( rv<0 ) {
            pstat->write_error ++;
        }
        pstat->write_t += check_point(now_us, now_sec, last_us);

        pmydata->seed_count = seed_count; // this statement must put here, otherwise read_lost got by read thread will be incorrect

        /* load control */
        while(conf_load_of_thread == 0 && pmydata->is_running) {
            usleep(50 * 1000);
        }
        usleep( (10 - conf_load_of_thread) * 0.5 * (pstat->write_t/pstat->write) );

    }

    return NULL;
}

void* thread_read(void* para) {
    int now_us = 0;
    int now_sec = 0;
    int last_us = 0;
    unsigned int key_seed;
    unsigned int seed_count_my = 0;
    unsigned int seed_count_to;
    std::set<std::string> keys;
    volatile work_thread_data_t *pmydata = (work_thread_data_t *)para;
    volatile stat_item_t        *pstat   = &pmydata->stat;

    while(pmydata->key_seed == THREAD_INITIAL_KEY_SEED) {
        sleep(1); //thread write is not ready
    }

    key_seed   = pmydata->key_seed;

    while( pmydata->is_running) {

        /* reproduce keys same as in thread_write */

        seed_count_to = pmydata->seed_count;
        while(seed_count_my < seed_count_to) {
            std::string key = get_random_key(key_seed, seed_count_my);
            keys.insert(key);
        }


        std::set<std::string>::iterator itkey = keys.begin();
        for(; itkey != keys.end(); itkey++) {
            const std::string &key = *itkey;

            /* get value from cache */
            std::string value_tobe;
            {
                redis::cluster::LockGuard lockguard(c_lock);
                CacheType::iterator iter = cache.find( key );
                if( iter != cache.end() )  {
                    value_tobe = iter->second;
                } else {
                    abort(); //not allowed
                }
            }

            /* read redis server */
            std::string value_read;
            check_point(now_us, now_sec, last_us);
            int rv = redis_get(key, value_read);
            pstat->read_ttl += cluster_->ttls();
            pstat->read ++;
            if( rv < 0 ) {
                pstat->read_error ++;
            } else if (rv == 1) {
                pstat->read_lost++;
            } else {
                if( value_read != value_tobe )  {
                    pstat->unmatch ++;
                }
            }
            pstat->read_t += check_point(now_us, now_sec, last_us);

            if(!pmydata->is_running) {
                break;
            }

            /* load control */
            while(conf_load_of_thread == 0 && pmydata->is_running) {
                usleep(50 * 1000);
            }
            usleep( (10 - conf_load_of_thread) * 0.5 * (pstat->read_t/pstat->read) );
        }

    }
    return NULL;
}

void* thread_conf(void* para) {

    int ch;
    initscr();
    cbreak();                 // no input buf
    noecho();                 // no echo
    keypad(stdscr, TRUE);     // enable function key
    refresh();                // clear screan
    //nl();

    while(true) {
        ch = getch();

        if (ch == 'q' or ch == 'Q') {
            endwin();
            conf_is_running = false;
            break;
        }

        if (ch == KEY_LEFT) {
            if(conf_threads_num > 0)
                conf_threads_num--;
        } else if (ch == KEY_RIGHT) {
            if(conf_threads_num < MAX_THREADS_NUM)
                conf_threads_num++;
        } else if (ch == KEY_UP) {
            if(conf_load_of_thread < MAX_LOAD_OF_THREAD)
                conf_load_of_thread++;
        } else if (ch == KEY_DOWN) {
            if(conf_load_of_thread > 0)
                conf_load_of_thread--;
        }
    }
    return NULL;
}

#ifdef SUPPORT_RRD
int create_rrd_ds() {
    char arg_start[50];
    time_t tm = time(NULL);
    std::vector<char *>rrd_argv;

    char arg_cmd[]= "create";
    rrd_argv.push_back(arg_cmd);
    rrd_argv.push_back(rrd_file);
    snprintf(arg_start, sizeof(arg_start), "-b %u", (unsigned int)tm);
    rrd_argv.push_back(arg_start);
    char arg_step[]="-s1";
    rrd_argv.push_back(arg_step);
    char ds_read[] = "DS:read:GAUGE:1:U:U";
    rrd_argv.push_back(ds_read);
    char ds_read_err[] = "DS:read_err:GAUGE:1:U:U";
    rrd_argv.push_back(ds_read_err);
    char ds_read_lost[] = "DS:read_lost:GAUGE:1:U:U";
    rrd_argv.push_back(ds_read_lost);
    char ds_read_unmatch[] = "DS:unmatch:GAUGE:1:U:U";
    rrd_argv.push_back(ds_read_unmatch);
    char ds_write[] = "DS:write:GAUGE:1:U:U";
    rrd_argv.push_back(ds_write);
    char ds_write_new[] = "DS:write_new:GAUGE:1:U:U";
    rrd_argv.push_back(ds_write_new);
    char ds_write_err[] = "DS:write_err:GAUGE:1:U:U";
    rrd_argv.push_back(ds_write_err);
    char ds_read_t[] = "DS:read_t:GAUGE:1:0:5000";
    rrd_argv.push_back(ds_read_t);
    char ds_write_t[] = "DS:write_t:GAUGE:1:0:5000";
    rrd_argv.push_back(ds_write_t);
    char arg_rra[] = "RRA:AVERAGE:0.5:2:3600";
    rrd_argv.push_back(arg_rra);

//        setlocale(LC_ALL, "");
    rrd_clear_error();
    int rc = rrd_create(rrd_argv.size(), rrd_argv.data());
    if(rc != 0) {
        std::cerr << rrd_get_error() << std::endl;
        return -1;
    }

    return 0;
}

void update_rrd_file(uint32_t now_sec, uint64_t read, uint64_t read_err, uint64_t read_lost, uint64_t unmatch,  // 1
                     uint64_t write, uint64_t write_new, uint64_t write_err, uint64_t read_t, uint64_t write_t // 2
                    ) {
    char rrd_arg[50];
    std::vector<char *>rrd_argv;

    char arg_cmd[]= "update";
    rrd_argv.push_back(arg_cmd);
    rrd_argv.push_back(rrd_file);
    snprintf(rrd_arg, sizeof(rrd_arg), "%u:%lu:%lu:%lu:%lu:%lu:" // 1
             "%lu:%lu:%lu:%lu", // 2
             (unsigned int)now_sec, read, read_err,read_lost,unmatch,  // 1
             write, write_new, write_err,read_t, write_t// 2
            );

    rrd_argv.push_back(rrd_arg);

    rrd_clear_error();
    int rc = rrd_update(rrd_argv.size(), rrd_argv.data());
    if(rc != 0) {
        std::cerr << rrd_get_error() << std::endl;
    }

}
void update_rrd_png(uint32_t now_sec) {

    static char rrd_png_read[]= "./infinite_read.png";
    static char rrd_png_write[]= "./infinite_write.png";
    static char rrd_png_rw_t[]= "./infinite_rw_t.png";


#define ADD_ARG_HEADER(file_) \
        std::vector<char *>rrd_argv;\
        char arg_cmd[]= "graph";\
        rrd_argv.push_back(arg_cmd);\
        rrd_argv.push_back(file_); \
        char arg_start[50]; \
        snprintf(arg_start, sizeof(arg_start), "-s now-300");\
        rrd_argv.push_back(arg_start);\
        char arg_width[] = "-w 600";\
        rrd_argv.push_back(arg_width);\
        char arg_height[] = "-h 300";\
        rrd_argv.push_back(arg_height);

#define UPDATE_RRD \
    char    **calcpr;\
    int       xsize, ysize;\
    double    ymin, ymax;\
    rrd_clear_error();\
    int rc = rrd_graph(rrd_argv.size(), rrd_argv.data(), &calcpr, &xsize, &ysize, NULL, &ymin, &ymax);\
    if(rc == 0) {\
        if (calcpr) {\
            for (int i = 0; calcpr[i]; i++) {\
                printf("%s\r\n", calcpr[i]);\
                free(calcpr[i]);\
            }\
            free(calcpr);\
        }\
    } else {\
        std::cerr << rrd_get_error() << std::endl;\
    }

#define _DEF_LINE(_name,_def,_vdef,_draw_type, _draw_detail)\
            char def##_name[50];\
            snprintf(def##_name, sizeof(def##_name), "DEF:" #_name "=%s:" _def, rrd_file);\
            rrd_argv.push_back(def##_name);\
            char draw##_name[] = _draw_type ":" #_name "#" _draw_detail ":" #_name;\
            rrd_argv.push_back(draw##_name);
#define DEF_LINE(_name,_def,_vdef,_draw_type, _draw_detail) _DEF_LINE(_name,_def,_vdef,_draw_type, _draw_detail)


    {
        /* read */
        ADD_ARG_HEADER(rrd_png_read);
        DEF_LINE(read,"read:AVERAGE",NULL,"LINE1","00FF00");
        DEF_LINE(read_err,"read_err:AVERAGE",NULL,"LINE1","FF0000");
        DEF_LINE(read_lost,"read_lost:AVERAGE",NULL,"LINE1","0000FF");
        DEF_LINE(unmatch,"unmatch:AVERAGE",NULL,"LINE1","F0F0F0");
        UPDATE_RRD
    }

    {
        /* write */
        ADD_ARG_HEADER(rrd_png_write);
        DEF_LINE(write,"write:AVERAGE", NULL,  "LINE1", "00FF00");
        DEF_LINE(write_new,"write_new:AVERAGE", NULL,  "LINE1", "0000FF");
        DEF_LINE(write_err,"write_err:AVERAGE", NULL,  "LINE1", "FF0000");
        UPDATE_RRD
    }

    {
        /* read_t and write_t */
        ADD_ARG_HEADER(rrd_png_rw_t);
        DEF_LINE(read_t,"read_t:AVERAGE", NULL,  "LINE1", "00FF00");
        DEF_LINE(write_t,"write_t:AVERAGE", NULL,  "LINE1", "0000FF");
        UPDATE_RRD
    }

#undef ADD_ARG_HEADER
#undef _DEF_LINE
#undef DEF_LINE
#undef UPDATE_RRD
}

#endif

int main(int argc, char *argv[]) {

    bool interactively = true;

    /* init cache */

    int ret = pthread_spin_init(&c_lock, PTHREAD_PROCESS_PRIVATE);
    if(ret != 0) {
        std::cerr << "pthread_spin_init fail" << std::endl;
        return 1;
    }

    /* init cluster */

    std::string startup = "127.0.0.1:7000,127.0.0.1:7001";
    char *arg_startup   = NULL;

    if( argc > 1 ) {
        for(int i = 1; i < argc; i++) {
            if(strncmp(argv[i], "-n", 2) == 0) {
                if(interactively) {
                    interactively = false;
                    int n = atoi(argv[i] + 2);
                    if(n > 0) {
                        conf_threads_num = n > MAX_THREADS_NUM? MAX_THREADS_NUM: n;
                    }
                }
            } else if(!arg_startup)
                arg_startup = argv[i];
        }
    }
    if(arg_startup)
        startup = arg_startup;

    std::cout << "cluster startup with " << startup << std::endl;
    cluster_ = new redis::cluster::Cluster(1);

    if( cluster_->setup(startup.c_str(), true)!=0 ) {
        std::cerr << "cluster setup fail" << std::endl;
        return 1;
    }

    /* create config thread */

    if(interactively) {
        std::cout << "Hotkeys: \r\n"
                  "'LEFT'/'RIGHT' key to decrease/increase number of running threads.\r\n"
                  "'UP'/'DOWN' key to turn up/down the Read and Write speeds per thread.\r\n"
                  "'q' to quit.\r\n"
                  "press ENTER to continue...";
        getchar();

        pthread_t thconf;
        if(pthread_create(&thconf, NULL, thread_conf, NULL) != 0) {
            std::cerr << "create config thread fail" << std::endl;
            return 1;
        }
    }

#ifdef SUPPORT_RRD

    if(create_rrd_ds() != 0) {
        return 1;
    }
#endif

    /* control work threads and do statistics */

    std::vector<work_thread_data_t> threads_data;
    std::vector<stat_item_t>        thread_stats_last;
    int                 workers_num_now;
    int                 workers_num_to;
    work_thread_data_t *pdata;
    int                 last_sec = 0;
#ifdef SUPPORT_RRD
    int                 last_rrd_png = 0;
#endif
    struct              timeval tv;

    uint64_t total_read  = 0;
    uint64_t total_write = 0;

    threads_data.reserve(MAX_THREADS_NUM);

    while(true) {
        workers_num_now = threads_data.size();
        if(!conf_is_running)
            workers_num_to = 0;
        else
            workers_num_to  = conf_threads_num;

        if(workers_num_to > workers_num_now) {
            /* create more work threads */

            threads_data.resize(workers_num_to);
            thread_stats_last.resize(workers_num_to, stat_item_initial);


            for(int idx = workers_num_now; idx < workers_num_to; idx++) {
                pdata = &(threads_data[idx]);
                pdata->key_seed   = THREAD_INITIAL_KEY_SEED;
                pdata->seed_count = 0;
                pdata->is_running = true;
                pdata->stat       = stat_item_initial;
                thread_stats_last[idx] = stat_item_initial;
                if(pthread_create(&pdata->tid_w, NULL, thread_write, pdata) != 0) {
                    std::cerr << "create write thread "<<idx<<" fail" << std::endl;
                    exit(0);
                }
                if(pthread_create(&pdata->tid_r, NULL, thread_read, pdata) != 0) {
                    std::cerr << "create read thread "<<idx<<" fail" << std::endl;
                    exit(0);
                }
            }
        } else if (workers_num_to < workers_num_now) {
            /* end some work threads */

            for(int idx = workers_num_to; idx < workers_num_now; idx++) {
                pdata = &(threads_data[idx]);
                pdata->is_running = false;
                if(pthread_join(pdata->tid_w, NULL) != 0) {
                    std::cerr << "join write thread "<<idx<<" fail" << std::endl;
                }
                if(pthread_join(pdata->tid_r, NULL) != 0) {
                    std::cerr << "join read thread "<<idx<<" fail" << std::endl;
                }
            }
            threads_data.resize(workers_num_to);
            thread_stats_last.resize(workers_num_to);
        }

        if(!conf_is_running)
            break;

        /* statistic */

        gettimeofday( &tv, NULL );
        int now_sec = tv.tv_sec;
        if( last_sec != now_sec ) {

            last_sec = now_sec;

            uint64_t read        = 0;
            uint64_t read_t      = 0;
            uint64_t read_ttl    = 0;
            uint64_t read_error  = 0;
            uint64_t read_lost   = 0;
            uint64_t unmatch     = 0;
            uint64_t write       = 0;
            uint64_t write_new   = 0;
            uint64_t write_t     = 0;
            uint64_t write_ttl   = 0;
            uint64_t write_error = 0;

            stat_item_t stat_now;

            for(int idx = 0; idx < workers_num_to; idx++) {
                stat_now = threads_data[idx].stat;

                read        += (stat_now.read        - thread_stats_last[idx].read);
                read_t      += (stat_now.read_t      - thread_stats_last[idx].read_t);
                read_ttl    += (stat_now.read_ttl    - thread_stats_last[idx].read_ttl);
                read_error  += (stat_now.read_error  - thread_stats_last[idx].read_error);
                read_lost   += (stat_now.read_lost   - thread_stats_last[idx].read_lost);
                unmatch     += (stat_now.unmatch     - thread_stats_last[idx].unmatch);
                write       += (stat_now.write       - thread_stats_last[idx].write);
                write_new   += (stat_now.write_new   - thread_stats_last[idx].write_new);
                write_t     += (stat_now.write_t     - thread_stats_last[idx].write_t);
                write_ttl   += (stat_now.write_ttl   - thread_stats_last[idx].write_ttl);
                write_error += (stat_now.write_error - thread_stats_last[idx].write_error);

                thread_stats_last[idx] = stat_now;
            }
            total_read  += read;
            total_write += write;

#ifdef SUPPORT_RRD

            update_rrd_file(now_sec, read, read_error,read_lost, unmatch, write, write_new, write_error,   // 1
                            (read?(read_t/read):0), (write?(write_t/write):0) // 2
                           );

            if(now_sec - last_rrd_png > 2) {
                if(last_rrd_png != 0) {
                    last_rrd_png = now_sec;
                    update_rrd_png(now_sec);
                } else {
                    last_rrd_png = now_sec;
                }
            }
#endif


            std::cout << conf_threads_num <<" threads " << conf_load_of_thread << " loads "
                      << total_read << " R("<<read<<" read, " << read_error << " err, "<< read_lost << " lost, " <<  unmatch << " unmatch, "
                      << (read?(read_t/read):0)<<" usec per op, "
                      << (read?(read_ttl/read):0)<<" ttls) | "

                      << total_write << " W(" << write << " write, " << write_error << " err, "<< write_new << " new, "
                      << (write?(write_t/write):0)<<" usec per op, "
                      << (write?(write_ttl/write):0)<<" ttls) " << cluster_->stat_dump() << "\r\n";

            fflush(stdout);

        }
        usleep( 1000*10 );
    }

    return 0;
}


