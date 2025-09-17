#include "include/opl.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/renderman.h"
#include "include/pad.h"
#include <pthread.h>

int ForceRefreshPrevTexCache = 0;
int forceSkipQr = 0;
int texLoading = 0;

static int PrevCacheID = -2;
static int PrevCacheID_COV = -2;
static int PrevCacheID_ICO = -2;
static int PrevCacheID_BG = -2;

//int artQrCount = 0; // 给加入Qr缓存队列的Art图计数
//int artQrDone = 0; // 代表一轮Art图已全部进入Qr队列
static int cdFrames = 30;         // 一轮Art图Qr后的CD时间(帧数)
static int cdFramesCount;         // 手动重复按键
static int buttonPressedOnce = 0; // 快速连按时，每次按键只重置CD帧数一次
//int buttonFrames = 0; // 按住按键的帧数，用来跳过cdFrames
static u64 prevGuiFrameId = 0; // 和guiFrameId进行比对，判断是否完成了一轮Qr
static int skipQr = 0;  // 判断是否可以跳过请求Qr队列
static char *curStartUp = NULL;
static int findBGCount = 0; // 寻找背景图的次数

typedef struct
{
    image_cache_t *cache;
    cache_entry_t *entry;
    item_list_t *list;
    // only for comparison if the deferred action is still valid
    int cacheUID;
    char *value;
} load_image_request_t;

static load_image_request_t *batchRequests[MENU_MIN_INACTIVE_FRAMES];
static int batchRequestCount = 0;
static int ioPendingRequestCount_be = -1;
static int ioPendingRequestCount_af = -1;

static void cacheClearItem(cache_entry_t *item, int freeTxt)
{
    if (freeTxt) {
        if (item->texture.Mem) {
            rmUnloadTexture(&item->texture);
            free(item->texture.Mem);
        }
        if (item->texture.Clut)
            free(item->texture.Clut);
    }

    memset(item, 0, sizeof(cache_entry_t));
    item->texture.Width = 0;            // Must be set by loader
    item->texture.Height = 0;           // Must be set by loader
    item->texture.PSM = GS_PSM_CT24;    // Must be set by loader
    item->texture.ClutPSM = 0;          // Default, can be set by loader
    item->texture.TBW = 0;              // gsKit internal value
    item->texture.Mem = NULL;           // Must be allocated by loader
    item->texture.Clut = NULL;          // Default, can be set by loader
    item->texture.Vram = 0;             // VRAM allocation handled by texture manager
    item->texture.VramClut = 0;         // VRAM allocation handled by texture manager
    item->texture.Filter = GS_FILTER_LINEAR; // Default
    // item->texture.ClutStorageMode = GS_CLUT_STORAGE_CSM1; // Default
    //  Do not load the texture to VRAM directly, only load it to EE RAM
    item->texture.Delayed = 1;

    item->qr = 0;
    item->lastUsed = 0;
    item->UID = -1;
    item->texFound = -1;
}

// Io handled action...
static void cacheLoadImage(void *data)
{
    //load_image_request_t **tempBatchRequests = (load_image_request_t **)data;
    int count = batchRequestCount;
    batchRequestCount = 0;
    for (int i = 0; i < count; i++) {
        load_image_request_t *req = batchRequests[i];

        // Safeguards...
        if (!req || !req->entry || !req->cache)
            continue;

        item_list_t *handler = req->list;
        if (!handler) {
            req->entry->qr = 0;
            free(req);
            batchRequests[i] = NULL; // 及时清理，避免野指针
            continue;
        }

        // the cache entry was already reused!
        if (req->cacheUID != req->entry->UID) {
            req->cacheUID = -1;
            req->entry->UID = -1;
            req->entry->qr = 0;
            free(req);
            batchRequests[i] = NULL; // 及时清理，避免野指针
            continue;
        }

        // 光标指向的游戏ID和后台加载的art图片不符时，或者已经处于CD(按住和快速点击)时，停止加载图片，避免卡顿
        // 中断读取，会引发UID混乱，同一个游戏有不同的UID，目前不知道会产生什么后果，也许没什么影响
        if (cdFramesCount) {
            req->entry->qr = 0;
            free(req);
            batchRequests[i] = NULL; // 及时清理，避免野指针
            continue;
        }

        //// seems okay. we can proceed
        // GSTEXTURE *texture = &req->entry->texture;
        // texFree(texture);

        if (handler->itemGetImage(handler, req->cache->prefix, req->cache->isPrefixRelative, req->value, req->cache->suffix, &req->entry->texture, GS_PSM_CT24) < 0) {
            req->entry->lastUsed = 0;
            req->entry->texFound = 0;
        }
        else {
            req->entry->lastUsed = guiFrameId;
            req->entry->texFound = 1;
        }
        req->entry->qr = 0;
        free(req);
        batchRequests[i] = NULL; // 及时清理，避免野指针
    }
    texLoading = 0;
    //return NULL;
}

void flushBatchRequests(void)
{
    // 有堆积的图片待加载
    if (batchRequestCount > 0 && !texLoading) {
        //// debug  打印debug信息
        //char debugFileDir[64];
        //strcpy(debugFileDir, "smb:debug-TexCacheAllArtIoOnce.txt");
        //FILE *debugFile = fopen(debugFileDir, "ab+");
        //if (debugFile != NULL) {
        //    fprintf(debugFile, "batchRequestCount:%d   guiFrameId:%d  curStartUp:%s\r\n", batchRequestCount, guiFrameId, curStartUp);
        //    fclose(debugFile);
        //}
        //  保证只存在一个io请求，多了会产生冲突导致死机
        // if (!texLoading)
        {
            texLoading = 1;
            //cacheLoadImage(NULL);

            // 使用官方的多线程方法 
            // ioPutRequest(IO_CACHE_LOAD_ART, batchRequests);
            ioPutRequest(IO_CACHE_LOAD_ART, NULL);

            // 使用pthread的多线程方法
            //pthread_t tid;
            //pthread_attr_t attr;
            //pthread_attr_init(&attr);

            //// 线程分离，如果不需要pthread_join
            //pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            //// 设置合适的栈空间，防止爆栈等错误
            //pthread_attr_setstacksize(&attr, 256 * 1024);

            //// 创建线程
            //pthread_create(&tid, &attr, cacheLoadImage, NULL);
            //pthread_attr_destroy(&attr);
        }
        // else {
        //     // 如果执行过程中突然又来一个io，就立刻中断io，清空堆积的请求
        //     for (int i = 0; i < batchRequestCount; i++) {
        //         if (batchRequests[i]) {
        //             batchRequests[i]->entry->UID = 0; // 也许这个不还原成0是最好的，让每个startup对应正确的UID，但这样最简单
        //             batchRequests[i]->entry->qr = NULL;
        //             free(batchRequests[i]);
        //             batchRequests[i] = NULL; // 可选，防止野指针
        //         }
        //     }
        //     batchRequestCount = 0;

        //    // 满足特定条件，触发连按CD
        //    if (!padGetRepeating() && !ForceRefreshPrevTexCache)
        //        cdFramesCount = 1; // 触发连按CD
        //}
    }
}

void cacheInit()
{
    ioRegisterHandler(IO_CACHE_LOAD_ART, &cacheLoadImage);
}

void cacheEnd()
{
    // nothing to do... others have to destroy the cache via cacheDestroyCache
}

image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count)
{
    image_cache_t *cache = (image_cache_t *)malloc(sizeof(image_cache_t));
    cache->userId = userId;
    cache->count = count;
    cache->prefix = NULL;
    int length;
    if (prefix) {
        length = strlen(prefix) + 1;
        cache->prefix = (char *)malloc(length * sizeof(char));
        memcpy(cache->prefix, prefix, length);
    }
    cache->isPrefixRelative = isPrefixRelative;
    length = strlen(suffix) + 1;
    cache->suffix = (char *)malloc(length * sizeof(char));
    memcpy(cache->suffix, suffix, length);
    cache->nextUID = 1;
    cache->content = (cache_entry_t *)malloc(count * sizeof(cache_entry_t));

    int i;
    for (i = 0; i < count; ++i)
        cacheClearItem(&cache->content[i], 0);

    return cache;
}

void cacheDestroyCache(image_cache_t *cache)
{
    int i;
    for (i = 0; i < cache->count; ++i) {
        cacheClearItem(&cache->content[i], 1);
    }

    free(cache->prefix);
    free(cache->suffix);
    free(cache->content);
    free(cache);
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    if (ioHasPendingRequests()) {
        ioPendingRequestCount_af = ioGetPendingRequestCount();
        // debug  打印debug信息
        char debugFileDir[64];
        strcpy(debugFileDir, "smb:debug-TexCachePendingRequests.txt");
        FILE *debugFile = fopen(debugFileDir, "ab+");
        if (debugFile != NULL) {
            fprintf(debugFile, "当前队列的io请求数量为：%d\r\n\r\n", ioPendingRequestCount_af);
            fclose(debugFile);
        }
    }
    // 启动id变化时，说明光标有移动（可能用UID判断，效率更高更合理，之后再改。UID一开始是-1，然后再分配一个正整数）
    if (curStartUp != value) {
        // 移动光标时，如果有IO请求，就会跳过Qr，后台也会停止继续加载队列中的图片
        if (curStartUp && !padGetRepeating() && !ForceRefreshPrevTexCache && texLoading)
            cdFramesCount = 1; // 触发连按CD
        curStartUp = value;
    }

    // 默认情况下，触发重复按键时，就会跳过所有Qr
    if (padGetRepeating()) {
        findBGCount = 0;
        cdFramesCount = 0; // 强制结束连按CD
    } else
        buttonPressedOnce = 0;
    skipQr = gScrollSpeed > 0 ? padGetRepeating() : 0;
    if (cdFramesCount) {
        //if (cdFramesCount == 1) {
        //    buttonPressedOnce = 1;
        //    cdFrames = 50; // 第一次触发时的CD会长一点，需要考虑loadtex的卡顿时间
        //    //// debug  打印debug信息
        //    //char debugFileDir[64];
        //    //strcpy(debugFileDir, "smb:debug-TexCacheIoPut.txt");
        //    //FILE *debugFile = fopen(debugFileDir, "ab+");
        //    //if (debugFile != NULL) {
        //    //    fprintf(debugFile, "artQrCount:%d   UID:%d   cacheID:%d\r\ncurStartUp:%s_%s\r\n\r\n", artQrCount ,* UID, *cacheId, curStartUp, cache->suffix);
        //    //    fclose(debugFile);
        //    //}
        //}

        // 连按CD期间，再次按键，重置帧数
        if (getKeyPressed(KEY_UP) || getKeyPressed(KEY_DOWN) || getKeyPressed(KEY_L1) || getKeyPressed(KEY_R1)) {
            cdFramesCount = 1;
            //// 按下按键只重置一次的变量
            //if (!buttonPressedOnce) {
            //    buttonPressedOnce = 1;
            //    cdFrames = 50;
            //}
        } else
            buttonPressedOnce = 0;

        // CD期间跳过Qr，防止卡顿，CD结束后恢复原状
        if (cdFramesCount++ <= cdFrames)
            skipQr = 1;
        else
            cdFramesCount = 0;

        // 下次第一个加载背景图，如果没有就重试N次后退出
        if (!cdFramesCount) {
            if (cache->suffix[0] != 'B') {
                cdFramesCount = 10000;
                if (++findBGCount >= MENU_MIN_INACTIVE_FRAMES) {
                    findBGCount = 0;
                    cdFramesCount = 0;
                } else
                    skipQr = 1;
            } else
                findBGCount = 0;
        }

        // CD期间进入了自动连按状态，矫正一次Qr，结束cdFramesCount
        if (gScrollSpeed > 0 && padGetRepeating()) {
            findBGCount = 0;
            cdFramesCount = 0;
            skipQr = 1;
        }
    }

    // 左右切页签强制刷新缓存的变量，需要判断当前游戏所有图片是否都处理完毕
    if ((ForceRefreshPrevTexCache > 1) && (prevGuiFrameId != guiFrameId))
        ForceRefreshPrevTexCache = 0;

    if (forceSkipQr)
        skipQr = 1;

    // 切换设备页签时，上次图缓存需要清掉
    if (ForceRefreshPrevTexCache) {
        if (ForceRefreshPrevTexCache == 1) {
            prevGuiFrameId = guiFrameId;
            ForceRefreshPrevTexCache++;
        }
        // 重置上次的缓存ID
        PrevCacheID = -2;
    } else {
        // 根据图像类型，赋值上一次的缓存
        if (!strncmp("COV", cache->suffix, 3))
            PrevCacheID = PrevCacheID_COV;
        else if (!strncmp("ICO", cache->suffix, 3))
            PrevCacheID = PrevCacheID_ICO;
        else if (!strncmp("BG", cache->suffix, 2))
            PrevCacheID = PrevCacheID_BG;
        else
            PrevCacheID = -2;
    }

    // -2代表无图像，-1代表正在查找图像，0-9代表缓存编号
    if (*cacheId == -2) {
        // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
        if (!strncmp("COV", cache->suffix, 3))
            PrevCacheID_COV = *cacheId;
        else if (!strncmp("ICO", cache->suffix, 3))
            PrevCacheID_ICO = *cacheId;
        else if (!strncmp("BG", cache->suffix, 2))
            PrevCacheID_BG = *cacheId;
        return NULL;
    } else if (*cacheId != -1) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if (entry->qr) {
                return PrevCacheID < 0 ? NULL : &cache->content[PrevCacheID].texture;
            } else if (entry->texFound == 0) {
                *cacheId = -2;
                // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
                if (!strncmp("COV", cache->suffix, 3))
                    PrevCacheID_COV = *cacheId;
                else if (!strncmp("ICO", cache->suffix, 3))
                    PrevCacheID_ICO = *cacheId;
                else if (!strncmp("BG", cache->suffix, 2))
                    PrevCacheID_BG = *cacheId;
                return NULL;
            } else if (entry->texFound == 1 && entry->texture.Mem) {
                entry->lastUsed = guiFrameId;
                // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
                if (!strncmp("COV", cache->suffix, 3))
                    PrevCacheID_COV = *cacheId;
                else if (!strncmp("ICO", cache->suffix, 3))
                    PrevCacheID_ICO = *cacheId;
                else if (!strncmp("BG", cache->suffix, 2))
                    PrevCacheID_BG = *cacheId;
                return &entry->texture;
                //else {
                //    if (!skipQr) {
                //        load_image_request_t *req = malloc(sizeof(load_image_request_t) + strlen(value) + 1);
                //        req->cache = cache;
                //        req->entry = entry;
                //        req->list = list;
                //        req->value = (char *)req + sizeof(load_image_request_t);
                //        strcpy(req->value, value);
                //        req->cacheUID = *UID;
                //        req->entry->qr = req;
                //        ioPutRequest(IO_CACHE_LOAD_ART, req);
                //        return PrevCacheID < 0 ? NULL : &cache->content[PrevCacheID].texture;
                //    }
                //}
            }
        }

        *cacheId = -1;
    }

    if (skipQr || texLoading)
        return PrevCacheID < 0 ? NULL : &cache->content[PrevCacheID].texture;

    cache_entry_t *currEntry, *oldestEntry = NULL;
    int i;
    u64 rtime = guiFrameId;

    // 寻找可替换的槽
    for (i = 0; i < cache->count; i++) {
        currEntry = &cache->content[i];
        // 可用槽，但需保护正在使用的
        if (!currEntry->qr && (currEntry->lastUsed < rtime) &&
            !(PrevCacheID >= 0 && (&currEntry->texture == &cache->content[PrevCacheID].texture))) {
            oldestEntry = currEntry;
            rtime = currEntry->lastUsed;
            *cacheId = i;
        }
    }

    if (oldestEntry) {
        load_image_request_t *req = malloc(sizeof(load_image_request_t));
        req->cache = cache;
        req->entry = oldestEntry;
        req->list = list;
        req->value = value;

        cacheClearItem(req->entry, 1);
        req->entry->qr = 1;

        // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
        if (*UID == -1)
            req->entry->UID = req->cacheUID = *UID = cache->nextUID++;
        else
            req->entry->UID = req->cacheUID = *UID;

        // prevGuiFrameId = guiFrameId;
        // artQrCount++;
        if (batchRequestCount < MENU_MIN_INACTIVE_FRAMES) {
            if (batchRequests[batchRequestCount])
                free(batchRequests[batchRequestCount]);
            batchRequests[batchRequestCount++] = req;
        }
        //ioPutRequest(IO_CACHE_LOAD_ART, req);
        //// debug  打印debug信息
        //char debugFileDir[64];
        //strcpy(debugFileDir, "smb:debug-TexCacheDebugUID.txt");
        //FILE *debugFile = fopen(debugFileDir, "ab+");
        //if (debugFile != NULL) {
        //    fprintf(debugFile, "UID:%d  nextUID:%d  cacheId:%d  %s_%s\r\n", *UID, cache->nextUID, *cacheId, value, cache->suffix);
        //    fclose(debugFile);
        //}
    }
    return PrevCacheID < 0 ? NULL : &cache->content[PrevCacheID].texture;
}
