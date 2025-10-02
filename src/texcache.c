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
volatile int forceSkipQr = 0;

static int PrevCacheID = -2;
static int PrevCacheID_COV = -2;
static int PrevCacheID_ICO = -2;
static int PrevCacheID_BG = -2;

//int artQrCount = 0; // 给加入Qr缓存队列的Art图计数
//int artQrDone = 0; // 代表一轮Art图已全部进入Qr队列
static int buttonPressedOnce = 0;  // 快速连按时，每次按键只重置CD帧数一次
static int cdFrames = 30;         // 一轮Art图Qr后的CD时间(帧数)
static int skipQr = 0;             // 判断是否可以跳过请求Qr队列
static volatile int cdFramesCount = 0; // 手动重复按键
volatile int texLoading = 0;
//int buttonFrames = 0; // 按住按键的帧数，用来跳过cdFrames
//static u64 prevGuiFrameId = 0; // 和guiFrameId进行比对，判断是否完成了一轮Qr
static char *curStartUp = NULL;
static int findBGCount = 0; // 寻找背景图的次数
static int usePthread = 1;  // 使用pthread多线程方法加载图片
static int texLoadingTimeOut = 0;  // 用于判断加载计数异常时，将texLoading置为0
static int texNeedUpdate = 1;
// 只给主线程使用和显示
GSTEXTURE texture1_show = {0};
GSTEXTURE texture2_show = {0};
GSTEXTURE texture3_show = {0};
// 给加载线程使用
GSTEXTURE texture1_load = {0};
GSTEXTURE texture2_load = {0};
GSTEXTURE texture3_load = {0};

//pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t tid1;
pthread_t tid2;
pthread_t tid3;
pthread_attr_t attr;
pthread_mutex_t texLoadingMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t wakeupMutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
    int qr;
    pthread_cond_t cond;
    image_cache_t *cache;
    item_list_t *list;
    int *cacheId;
    char *value;
} load_image_request_t;
load_image_request_t req1;
load_image_request_t req2;
load_image_request_t req3;

static void cacheTexFree(GSTEXTURE *tex, int freeTxt)
{
    if (freeTxt) {
        if (tex->Mem) {
            rmUnloadTexture(tex);
            free(tex->Mem);
            tex->Mem = NULL; // Must be allocated by loader
            if (tex->Clut) {
                free(tex->Clut);
                tex->Clut = NULL; // Default, can be set by loader
            }
        }
    }
    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Mem = NULL;                // Must be allocated by loader
    tex->Clut = NULL;               // Default, can be set by loader
    tex->Width = 0;                 // Must be set by loader
    tex->Height = 0;                // Must be set by loader
    tex->PSM = GS_PSM_CT24;         // Must be set by loader
    tex->ClutPSM = 0;               // Default, can be set by loader
    tex->TBW = 0;                   // gsKit internal value
    tex->Vram = 0;                  // VRAM allocation handled by texture manager
    tex->VramClut = 0;              // VRAM allocation handled by texture manager
    tex->Filter = GS_FILTER_LINEAR; // Default
    // tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1; // Default
    //  Do not load the texture to VRAM directly, only load it to EE RAM
    tex->Delayed = 1;
}
static void cacheClearItem(cache_entry_t *item, int freeTxt)
{
    if (!item)
        return;

    //if (freeTxt) {
    //    if (item->texture.Mem) {
    //        WaitSema(fileLockId);
    //        rmUnloadTexture(&item->texture);
    //        free(item->texture.Mem);
    //        item->texture.Mem = NULL; // Must be allocated by loader
    //        SignalSema(fileLockId);
    //    }
    //    if (item->texture.Clut) {
    //        WaitSema(fileLockId);
    //        free(item->texture.Clut);
    //        item->texture.Clut = NULL; // Default, can be set by loader
    //        SignalSema(fileLockId);
    //    }
    //}

    memset(item, 0, sizeof(cache_entry_t));
    //item->texture.Width = 0;            // Must be set by loader
    //item->texture.Height = 0;           // Must be set by loader
    //item->texture.PSM = GS_PSM_CT24;    // Must be set by loader
    //item->texture.ClutPSM = 0;          // Default, can be set by loader
    //item->texture.TBW = 0;              // gsKit internal value
    //item->texture.Vram = 0;             // VRAM allocation handled by texture manager
    //item->texture.VramClut = 0;         // VRAM allocation handled by texture manager
    //item->texture.Filter = GS_FILTER_LINEAR; // Default
    //// item->texture.ClutStorageMode = GS_CLUT_STORAGE_CSM1; // Default
    ////  Do not load the texture to VRAM directly, only load it to EE RAM
    //item->texture.Delayed = 1;

    item->qr = 0;
    item->lastUsed = 0;
    item->UID = -1;
    item->texFound = -1;
}

static void *cacheLoadImage2(void *data)
{
     load_image_request_t *ioReq = (load_image_request_t *)data;
     while (1) {
         if (forceSkipQr)
             return NULL;
         // 等待激活
         ioReq->qr = 0;
         pthread_cond_wait(&ioReq->cond, &wakeupMutex);
         if (forceSkipQr)
             return NULL;

        // Safeguards...
        if (!ioReq->cache || !ioReq->cache->content) {
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading > 0)
                texLoading--;
            pthread_mutex_unlock(&texLoadingMutex);
            continue;
        }

        item_list_t *handler = ioReq->list;
        if (!handler) {
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading > 0)
                texLoading--;
            pthread_mutex_unlock(&texLoadingMutex);
            continue;
        }

        // 光标指向的游戏ID和后台加载的art图片不符时，或者已经处于CD(按住和快速点击)时，停止加载图片，避免卡顿
        if (cdFramesCount || forceSkipQr) {
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading > 0)
                texLoading--;
            pthread_mutex_unlock(&texLoadingMutex);
            continue;
        }

        // 加载图片
        int result = handler->itemGetImage(handler, "ART", 1, ioReq->value, ioReq->cache->suffix, ioReq->cache->content[0].texture, GS_PSM_CT24);

        if (result < 0) {
            WaitSema(fileLockId);
            *ioReq->cacheId = -2;
            if (!strncmp("BG", ioReq->cache->suffix, 2)) {
                cacheTexFree(&texture1_show, 1);
            } else if (!strncmp("COV", ioReq->cache->suffix, 3)) {
                cacheTexFree(&texture2_show, 1);
            } else if (!strncmp("ICO", ioReq->cache->suffix, 3)) {
                cacheTexFree(&texture3_show, 1);
            }
            SignalSema(fileLockId);
        } else {
            WaitSema(fileLockId);
            if (!strncmp("BG", ioReq->cache->suffix, 2)) {
                cacheTexFree(&texture1_show, 1);
                texture1_show = *ioReq->cache->content[0].texture;
                cacheTexFree(ioReq->cache->content[0].texture, 0);
            } else if (!strncmp("COV", ioReq->cache->suffix, 3)) {
                cacheTexFree(&texture2_show, 1);
                texture2_show = *ioReq->cache->content[0].texture;
                cacheTexFree(ioReq->cache->content[0].texture, 0);
            } else if (!strncmp("ICO", ioReq->cache->suffix, 3)) {
                cacheTexFree(&texture3_show, 1);
                texture3_show = *ioReq->cache->content[0].texture;
                cacheTexFree(ioReq->cache->content[0].texture, 0);
            }
            SignalSema(fileLockId);
        }
        pthread_mutex_lock(&texLoadingMutex);
        if (texLoading > 0)
            texLoading--;
        pthread_mutex_unlock(&texLoadingMutex);
     }
    return NULL;
}

//// Io handled action...
//static void *cacheLoadImage(void *data)
//{
//    pthread_mutex_lock(&texLoadingMutex);
//    load_image_request_t *ioReq = (load_image_request_t *)data;
//    // Safeguards...
//    if (!ioReq->cache || !ioReq->cache->content) {
//        if (texLoading)
//            texLoading--;
//        pthread_mutex_unlock(&texLoadingMutex);
//        free(ioReq);
//        return NULL;
//    }
//
//    item_list_t *handler = ioReq->list;
//    if (!handler) {
//        ioReq->cache->content[ioReq->cacheId].qr = 0;
//        if (texLoading)
//            texLoading--;
//        pthread_mutex_unlock(&texLoadingMutex);
//        free(ioReq);
//        return NULL;
//    }
//
//    // 光标指向的游戏ID和后台加载的art图片不符时，或者已经处于CD(按住和快速点击)时，停止加载图片，避免卡顿
//    if (cdFramesCount || forceSkipQr) {
//        ioReq->cache->content[ioReq->cacheId].qr = 0;
//        if (texLoading)
//            texLoading--;
//        pthread_mutex_unlock(&texLoadingMutex);
//        free(ioReq);
//        return NULL;
//    }
//    pthread_mutex_unlock(&texLoadingMutex);
//
//    // 加载图片
//    int result = handler->itemGetImage(handler, ioReq->cache->prefix, ioReq->cache->isPrefixRelative, ioReq->value, ioReq->cache->suffix, &ioReq->cache->content[ioReq->cacheId].texture, GS_PSM_CT24);
//
//    pthread_mutex_lock(&texLoadingMutex);
//    if (result < 0) {
//        ioReq->cache->content[ioReq->cacheId].lastUsed = 0;
//        ioReq->cache->content[ioReq->cacheId].texFound = 0;
//    } else {
//        ioReq->cache->content[ioReq->cacheId].lastUsed = guiFrameId;
//        ioReq->cache->content[ioReq->cacheId].texFound = 1;
//    }
//    ioReq->cache->content[ioReq->cacheId].qr = 0;
//    if (texLoading)
//        texLoading--;
//    pthread_mutex_unlock(&texLoadingMutex);
//    free(ioReq);
//    return NULL;
//}
//static void cacheLoadImage_Official(void *data)
//{
//    cacheLoadImage(data);
//}

void flushBatchRequests(void)
{
    // 左右切页签强制刷新缓存的变量，需要判断当前游戏所有图片是否都处理完毕
    if (ForceRefreshPrevTexCache > 1)
        ForceRefreshPrevTexCache = 0;

    // texLoading状态异常时，将texLoading置为0(补救措施)
    if (texLoading) {
        if (++texLoadingTimeOut >= 600 && !padGetRepeating()) // 没有按住按键，且加载超过10秒时，重置texLoading
            texLoading = 0;
    } else
        texLoadingTimeOut = 0;

    texNeedUpdate = 0;
    //// 有堆积的图片待加载
    //if (batchRequestCount > 0 && !texLoading) {
    //    //// debug  打印debug信息
    //    //char debugFileDir[64];
    //    //strcpy(debugFileDir, "smb:debug-TexCacheAllArtIoOnce.txt");
    //    //FILE *debugFile = fopen(debugFileDir, "ab+");
    //    //if (debugFile != NULL) {
    //    //    fprintf(debugFile, "batchRequestCount:%d   guiFrameId:%d  curStartUp:%s\r\n", batchRequestCount, guiFrameId, curStartUp);
    //    //    fclose(debugFile);
    //    //}

    //    //  使用官方的多线程方法
    //    ioRequestCount = batchRequestCount;
    //    batchRequestCount = 0;
    //    texLoading = 1;

    //    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &cacheLoadImage);

    //    //// 使用ptheard来推送
    //    ////pthread_mutex_lock(&mutex);
    //    //ioRequestCount = batchRequestCount;
    //    //batchRequestCount = 0;
    //    //texLoading = 1;
    //    //pthread_cond_signal(&cond);
    //    ////pthread_mutex_unlock(&mutex);
    //}
}

void cacheInit()
{
    // 初始化pthread线程属性
    pthread_attr_init(&attr);

    //// 线程分离，如果不需要pthread_join
    //pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    // 设置合适的栈空间，防止爆栈等错误
    pthread_attr_setstacksize(&attr, 2048 * 1024); // kb

    req1.cond = PTHREAD_COND_INITIALIZER;
    req2.cond = PTHREAD_COND_INITIALIZER;
    req3.cond = PTHREAD_COND_INITIALIZER;

    pthread_create(&tid1, &attr, cacheLoadImage2, &req1);
    pthread_create(&tid2, &attr, cacheLoadImage2, &req2);
    pthread_create(&tid3, &attr, cacheLoadImage2, &req3);
    //// 初始化图像结构体，防止double free
    //memset(&texture1_show, 0, sizeof(GSTEXTURE));
    //memset(&texture2_show, 0, sizeof(GSTEXTURE));
    //memset(&texture3_show, 0, sizeof(GSTEXTURE));
    //memset(&texture1_load, 0, sizeof(GSTEXTURE));
    //memset(&texture2_load, 0, sizeof(GSTEXTURE));
    //memset(&texture3_load, 0, sizeof(GSTEXTURE));

    //if (!usePthread)
    //    ioRegisterHandler(IO_CACHE_LOAD_ART, &cacheLoadImage_Official);
    //else {
    //    // 初始化pthread线程属性
    //    pthread_attr_init(&attr);

    //    // 线程分离，如果不需要pthread_join
    //    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    //    // 设置合适的栈空间，防止爆栈等错误
    //    pthread_attr_setstacksize(&attr, 32 * 1024); // kb
    //};
}

void cacheEnd()
{
    // nothing to do... others have to destroy the cache via cacheDestroyCache
    forceSkipQr = 1;
    pthread_cond_signal(&req1.cond);
    pthread_join(tid1, NULL); // 等待线程结束
    pthread_cond_signal(&req2.cond);
    pthread_join(tid2, NULL); // 等待线程结束
    //pthread_cond_signal(&req3.cond);
    //pthread_join(tid3, NULL); // 等待线程结束
    //pthread_attr_destroy(&attr);
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
    // 默认情况下，触发重复按键时，就会跳过所有Qr
    if (padGetRepeating()) {
        findBGCount = 0;
        cdFramesCount = 0; // 强制结束连按CD
    } else
        buttonPressedOnce = 0;
    skipQr = gScrollSpeed > 0 ? padGetRepeating() : 0;

    // 启动id变化时，说明光标有移动（可能用UID判断，效率更高更合理，之后再改。UID一开始是-1，然后再分配一个正整数）
    if (curStartUp != value) {
        // 移动光标时，如果有IO请求，就会跳过Qr，后台也会停止继续加载队列中的图片
        if (curStartUp && !ForceRefreshPrevTexCache && (ioHasPendingRequests() || texLoading > 0)) {
            if (!padGetRepeating())
                cdFramesCount = 1; // 触发连按CD
            else
                skipQr = 1; // 按住时，还有图片请求，就跳过本次Qr
        }
        curStartUp = value;
        texNeedUpdate = 1;
    }

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
                    texNeedUpdate = 1; // 恢复读取图片
                } else
                    skipQr = 1;
            } else {
                findBGCount = 0;
                texNeedUpdate = 1; // 恢复读取图片
            }
        }

        // CD期间进入了自动连按状态，矫正一次Qr，结束cdFramesCount
        if (gScrollSpeed > 0 && padGetRepeating()) {
            findBGCount = 0;
            cdFramesCount = 0;
            skipQr = 1;
        }
    }

    if (forceSkipQr)
        skipQr = 1;

    //else if (*cacheId != -1) {
    //    cache_entry_t *entry = &cache->content[*cacheId];
    //    if (entry) {
    //        if (entry->UID == *UID) {
    //            if (entry->qr) {
    //                return PrevCacheID < 0 ? NULL : &cache->content[PrevCacheID].texture;
    //            } else if (entry->texFound == 0) {
    //                *cacheId = -2;
    //                // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
    //                if (!strncmp("COV", cache->suffix, 3))
    //                    PrevCacheID_COV = *cacheId;
    //                else if (!strncmp("ICO", cache->suffix, 3))
    //                    PrevCacheID_ICO = *cacheId;
    //                else if (!strncmp("BG", cache->suffix, 2))
    //                    PrevCacheID_BG = *cacheId;
    //                return NULL;
    //            } else if (entry->texFound == 1) {
    //                if (&entry->texture && entry->texture.Mem) {
    //                    //entry->lastUsed = guiFrameId;
    //                    // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
    //                    if (!strncmp("COV", cache->suffix, 3))
    //                        PrevCacheID_COV = *cacheId;
    //                    else if (!strncmp("ICO", cache->suffix, 3))
    //                        PrevCacheID_ICO = *cacheId;
    //                    else if (!strncmp("BG", cache->suffix, 2))
    //                        PrevCacheID_BG = *cacheId;
    //                    return &entry->texture;
    //                }
    //            }
    //        }
    //    }
    //    *cacheId = -1;
    //}
    GSTEXTURE *curTex = NULL;
    if (!strncmp("BG", cache->suffix, 2))
        curTex = &texture1_show;
    else if(!strncmp("COV", cache->suffix, 3))
        curTex = &texture2_show;
    else if (!strncmp("ICO", cache->suffix, 3))
        curTex = &texture3_show; 

    // 切换设备页签时，上次图缓存需要清掉
    if (ForceRefreshPrevTexCache) {
        ForceRefreshPrevTexCache++;

        // 重置上次的缓存
        cacheTexFree(curTex, 1);
        curTex = NULL;
    }

    // -2代表无图像，-1代表正在查找图像，0-9代表缓存编号
    if (*cacheId == -2) {
        cacheTexFree(curTex, 1);
        curTex = NULL;
        return NULL;
    }

    if (!texNeedUpdate || skipQr)
        return curTex && curTex->Mem ? curTex : NULL;

    //if (skipQr || texLoading >= 3)
    //    return curTex && curTex->Mem ? curTex : NULL;

    //cache_entry_t *currEntry, *oldestEntry = NULL;
    //int i;
    //u64 rtime = guiFrameId;

    //// 寻找可替换的槽
    //for (i = 0; i < cache->count; i++) {
    //    currEntry = &cache->content[i];
    //    // 可用槽，但需保护正在使用的
    //    if (!currEntry->qr && (currEntry->lastUsed < rtime) &&
    //        !(PrevCacheID >= 0 && (&currEntry->texture) && (&currEntry->texture == &cache->content[PrevCacheID].texture))) {
    //        oldestEntry = currEntry;
    //        rtime = currEntry->lastUsed;
    //        *cacheId = i;
    //    }
    //}
    if (texNeedUpdate) {
        cache_entry_t *currEntry = &cache->content[0];

        //  加载图片
        if (!strncmp("BG", cache->suffix, 2) && !req1.qr) {
            req1.qr = 1;
            // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
            if (*UID == -1)
                currEntry->UID = *UID = cache->nextUID++;
            else
                currEntry->UID = *UID;
            cacheTexFree(&texture1_load, 1);
            currEntry->texture = &texture1_load;

            //  使用pthread的多线程方法
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading >= 0)
                texLoading++;
            else
                texLoading = 1;
            pthread_mutex_unlock(&texLoadingMutex);
            req1.cache = cache;
            req1.cacheId = cacheId;
            req1.list = list;
            req1.value = value;
            pthread_cond_signal(&req1.cond);
        } else if (!strncmp("COV", cache->suffix, 3) && !req2.qr) {
            req2.qr = 1;
            // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
            if (*UID == -1)
                currEntry->UID = *UID = cache->nextUID++;
            else
                currEntry->UID = *UID;
            cacheTexFree(&texture2_load, 1);
            currEntry->texture = &texture2_load;

            //  使用pthread的多线程方法
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading >= 0)
                texLoading++;
            else
                texLoading = 1;
            pthread_mutex_unlock(&texLoadingMutex);
            req2.cache = cache;
            req2.cacheId = cacheId;
            req2.list = list;
            req2.value = value;
            pthread_cond_signal(&req2.cond);
        } else if (!strncmp("ICO", cache->suffix, 3) && !req3.qr) {
            req3.qr = 1;
            // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
            if (*UID == -1)
                currEntry->UID = *UID = cache->nextUID++;
            else
                currEntry->UID = *UID;
            cacheTexFree(&texture3_load, 1);
            currEntry->texture = &texture3_load;

            //  使用pthread的多线程方法
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading >= 0)
                texLoading++;
            else
                texLoading = 1;
            pthread_mutex_unlock(&texLoadingMutex);
            req3.cache = cache;
            req3.cacheId = cacheId;
            req3.list = list;
            req3.value = value;
            pthread_cond_signal(&req3.cond);
        }

        //// debug  打印debug信息
        // char debugFileDir[64];
        // strcpy(debugFileDir, "smb:debug-currEntry.txt");
        // FILE *debugFile = fopen(debugFileDir, "ab+");
        // if (debugFile != NULL) {
        //     fprintf(debugFile, "%s_%s\r\n", cache->suffix, value);
        //     fclose(debugFile);
        // }
    }
    return curTex && curTex->Mem ? curTex : NULL;
}
