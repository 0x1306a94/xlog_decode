// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

/*
  *      Author: zhangchangrong
 */

#include "argparse/argparse.h"
#include "cwalk/include/cwalk.h"
#include "micro-ecc-master/uECC.h"
#include "zlib.h"
#include "zstd/lib/zstd.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern int errno;

static const char *const usage[] = {
    "xlog_decode [options] [[--] args]",
    "xlog_decode [options]",
    NULL,
};

//typedef enum {
//    false,
//    true
//} bool;

const int MAGIC_CRYPT_START = 0x01;
const int MAGIC_COMPRESS_CRYPT_START = 0x02;

const int NEW_MAGIC_CRYPT_START = 0x03;
const int NEW_MAGIC_COMPRESS_CRYPT_START = 0x04;
const int NEW_MAGIC_COMPRESS_CRYPT_START1 = 0x05;

const int MAGIC_SYNC_ZLIB_START = 0x06;
const int MAGIC_SYNC_NO_CRYPT_ZLIB_START = 0x08;
const int MAGIC_ASYNC_ZLIB_START = 0x07;
const int MAGIC_ASYNC_NO_CRYPT_ZLIB_START = 0x09;

const int MAGIC_SYNC_ZSTD_START = 0x0A;
const int MAGIC_SYNC_NO_CRYPT_ZSTD_START = 0x0B;
const int MAGIC_ASYNC_ZSTD_START = 0x0C;
const int MAGIC_ASYNC_NO_CRYPT_ZSTD_START = 0x0D;

const int MAGIC_END = 0x00;
const int BASE_KEY = 0xCC;

int lastseq = 0;

const char *PRIV_KEY = NULL;
const char *PUB_KEY = NULL;

const int TEA_BLOCK_LEN = 8;

bool Hex2Buffer(const char *str, size_t len, unsigned char *buffer) {
    if (NULL == str || len == 0 || len % 2 != 0) {
        return -1;
    }

    char tmp[3] = {0};
    size_t i;
    for (i = 0; i < len - 1; i += 2) {
        size_t j;
        for (j = 0; j < 2; ++j) {
            tmp[j] = str[i + j];
            if (!(('0' <= tmp[j] && tmp[j] <= '9') ||
                  ('a' <= tmp[j] && tmp[j] <= 'f') ||
                  ('A' <= tmp[j] && tmp[j] <= 'F'))) {
                return false;
            }
        }

        buffer[i / 2] = (unsigned char)strtol(tmp, NULL, 16);
    }
    return true;
}

void teaDecrypt(uint32_t *v, uint32_t *k) {
    uint32_t v0 = v[0], v1 = v[1], sum, i;
    const static uint32_t delta = 0x9e3779b9;
    const static uint32_t totalSum = 0x9e3779b9 << 4;
    sum = totalSum;

    uint32_t k0 = k[0], k1 = k[1], k2 = k[2], k3 = k[3];
    for (i = 0; i < 16; i++) {
        v1 -= ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
        v0 -= ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
        sum -= delta;
    }
    v[0] = v0;
    v[1] = v1;
}

bool isGoodLogBuffer(const char *buffer, size_t bufferSize, size_t offset, int count) {
    if (offset == bufferSize) {
        return true;
    }

    int cryptKeyLen = 0;
    int headerLen = 0;
    if (MAGIC_CRYPT_START == buffer[offset] || MAGIC_COMPRESS_CRYPT_START == buffer[offset]) {
        headerLen = 1 + 4;
    } else if (NEW_MAGIC_CRYPT_START == buffer[offset] ||
               NEW_MAGIC_COMPRESS_CRYPT_START == buffer[offset] ||
               NEW_MAGIC_COMPRESS_CRYPT_START1 == buffer[offset]) {
        headerLen = 1 + 2 + 1 + 1 + 4;
    } else if (MAGIC_ASYNC_ZLIB_START == buffer[offset] ||
               MAGIC_SYNC_ZLIB_START == buffer[offset] ||
               MAGIC_SYNC_NO_CRYPT_ZLIB_START == buffer[offset] ||
               MAGIC_ASYNC_NO_CRYPT_ZLIB_START == buffer[offset] ||
               MAGIC_ASYNC_ZSTD_START == buffer[offset] ||
               MAGIC_SYNC_ZSTD_START == buffer[offset] ||
               MAGIC_SYNC_NO_CRYPT_ZSTD_START == buffer[offset] ||
               MAGIC_ASYNC_NO_CRYPT_ZSTD_START == buffer[offset]) {
        headerLen = 1 + 2 + 1 + 1 + 4 + 64;
        cryptKeyLen = 64;
    } else {
        // '_buffer[%d]:%d != MAGIC_NUM_START'%(_offset, _buffer[_offset]))
        return false;
    }

    if (offset + headerLen + 1 + 1 > bufferSize) {
        // 'offset:%d > len(buffer):%d'%(_offset, len(_buffer))
        return false;
    }

    uint32_t length;
    memcpy(&length, &buffer[offset + headerLen - cryptKeyLen - 4], 4);

    if (offset + headerLen + length + 1 > bufferSize) {
        // 'log length:%d, end pos %d > len(buffer):%d' % (length, _offset + headerLen + length + 1, len(_buffer))
        return false;
    }
    if (MAGIC_END != buffer[offset + headerLen + length]) {
        // 'log length:%d, buffer[%d]:%d != MAGIC_END'%(length, _offset + headerLen + length, _buffer[_offset + headerLen + length])
        return false;
    }

    if (1 >= count) {
        return true;
    } else {
        return isGoodLogBuffer(buffer, bufferSize, offset + headerLen + length + 1, count - 1);
    }

    return true;
}

size_t getLogStartPos(const char *buffer, size_t bufferSize, int count) {
    size_t offset = 0;
    while (1) {
        if (offset >= bufferSize) {
            break;
        }
        if (buffer[offset] >= MAGIC_CRYPT_START && buffer[offset] <= MAGIC_ASYNC_NO_CRYPT_ZSTD_START) {
            if (isGoodLogBuffer(buffer, bufferSize, offset, count)) {
                return offset;
            }
        }
        offset += 1;
    }
    return -1;
}

void appendBuffer(char **outBuffer, size_t *outBufferSize, size_t *writePos, const char *buffer, size_t bufferSize) {
    if ((*outBufferSize) - (*writePos) < bufferSize + 1)  // + 1 for last \0
    {
        char *newOutBuffer = (char *)realloc(*outBuffer, (*outBufferSize) + 2 * bufferSize);
        if (NULL != newOutBuffer) {
            *outBuffer = newOutBuffer;
            *outBufferSize = (*outBufferSize) + 2 * bufferSize;
        } else {
            free(*outBuffer);
            fputs("Error reallocating memory\n", stderr);
            exit(5);
        }
    }

    memcpy(*outBuffer + (*writePos), buffer, bufferSize);
    *writePos = (*writePos) + bufferSize;
}

bool zstdDecompress(const char *compressedBytes, size_t compressedBytesSize, char **outBuffer, size_t *outBufferSize) {
    *outBuffer = NULL;
    *outBufferSize = 0;
    if (compressedBytesSize == 0) {
        return true;
    }

    unsigned fullLength = compressedBytesSize;
    unsigned halfLength = compressedBytesSize / 2;

    unsigned uncompLength = fullLength;
    char *uncomp = (char *)calloc(sizeof(char), uncompLength);

    ZSTD_DCtx *const dctx = ZSTD_createDCtx();

    ZSTD_inBuffer input = {compressedBytes, compressedBytesSize, 0};
    ZSTD_outBuffer output = {NULL, compressedBytesSize, 0};
    bool done = false;

    while (!done) {
        if (output.pos >= uncompLength) {
            char *uncomp2 = (char *)calloc(sizeof(char), uncompLength + halfLength);
            memcpy(uncomp2, uncomp, uncompLength);
            uncompLength += halfLength;
            free(uncomp);
            uncomp = uncomp2;
        }

        output.size = uncompLength;
        output.dst = uncomp;
        ZSTD_decompressStream(dctx, &output, &input);

        if (input.pos == input.size) {
            done = true;
        }
    }

    ZSTD_freeDCtx(dctx);

    *outBuffer = uncomp;
    *outBufferSize = output.pos;
    return true;
}

bool zlibDecompress(const char *compressedBytes, size_t compressedBytesSize, char **outBuffer, size_t *outBufferSize) {

    *outBuffer = NULL;
    *outBufferSize = 0;
    if (compressedBytesSize == 0) {
        return true;
    }

    unsigned fullLength = compressedBytesSize;
    unsigned halfLength = compressedBytesSize / 2;

    unsigned uncompLength = fullLength;
    char *uncomp = (char *)calloc(sizeof(char), uncompLength);

    z_stream strm;
    strm.next_in = (Bytef *)compressedBytes;
    strm.avail_in = compressedBytesSize;
    strm.total_out = 0;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;

    bool done = false;

    if (inflateInit2(&strm, (-MAX_WBITS)) != Z_OK) {
        free(uncomp);
        return false;
    }

    while (!done) {
        // If our output buffer is too small
        if (strm.total_out >= uncompLength) {
            // Increase size of output buffer
            char *uncomp2 = (char *)calloc(sizeof(char), uncompLength + halfLength);
            memcpy(uncomp2, uncomp, uncompLength);
            uncompLength += halfLength;
            free(uncomp);
            uncomp = uncomp2;
        }

        strm.next_out = (Bytef *)(uncomp + strm.total_out);
        strm.avail_out = uncompLength - strm.total_out;

        // Inflate another chunk.
        int err = inflate(&strm, Z_SYNC_FLUSH);
        if (err == Z_STREAM_END) {
            done = true;
        } else if (err != Z_OK) {
            break;
        }
    }

    if (inflateEnd(&strm) != Z_OK) {
        free(uncomp);
        return false;
    }

    *outBuffer = uncomp;
    *outBufferSize = strm.total_out;
    return true;
}

int decodeBuffer(const char *buffer, size_t bufferSize, size_t offset, char **outBuffer, size_t *outBufferSize, size_t *writePos) {
    if (offset >= bufferSize) {
        return -1;
    }

    if (!isGoodLogBuffer(buffer, bufferSize, offset, 1)) {
        size_t fixpos = getLogStartPos(buffer + offset, bufferSize - offset, 1);
        if (-1 == fixpos) {
            return -1;
        } else {
            char text[128];
            snprintf(text, sizeof(text), "[F]decode_log_file.py decode error len=%d\n", (int)fixpos);
            appendBuffer(outBuffer, outBufferSize, writePos, text, strlen(text));
            offset += fixpos;
        }
    }

    size_t cryptKeyLen = 0;
    size_t headerLen = 0;
    if (MAGIC_CRYPT_START == buffer[offset] || MAGIC_COMPRESS_CRYPT_START == buffer[offset]) {
        headerLen = 1 + 4;
    } else if (NEW_MAGIC_CRYPT_START == buffer[offset] ||
               NEW_MAGIC_COMPRESS_CRYPT_START == buffer[offset] ||
               NEW_MAGIC_COMPRESS_CRYPT_START1 == buffer[offset]) {
        headerLen = 1 + 2 + 1 + 1 + 4;
    } else if (MAGIC_ASYNC_ZLIB_START == buffer[offset] ||
               MAGIC_SYNC_ZLIB_START == buffer[offset] ||
               MAGIC_SYNC_NO_CRYPT_ZLIB_START == buffer[offset] ||
               MAGIC_ASYNC_NO_CRYPT_ZLIB_START == buffer[offset] ||
               MAGIC_ASYNC_ZSTD_START == buffer[offset] ||
               MAGIC_SYNC_ZSTD_START == buffer[offset] ||
               MAGIC_SYNC_NO_CRYPT_ZSTD_START == buffer[offset] ||
               MAGIC_ASYNC_NO_CRYPT_ZSTD_START == buffer[offset]) {
        headerLen = 1 + 2 + 1 + 1 + 4 + 64;
        cryptKeyLen = 64;
    } else {
        char text[128];
        snprintf(text, sizeof(text), "in DecodeBuffer _buffer[%zu]:%d != MAGIC_NUM_START", offset, (int)buffer[offset]);
        appendBuffer(outBuffer, outBufferSize, writePos, text, strlen(text));
        return -1;
    }

    uint32_t length;
    memcpy(&length, &buffer[offset + headerLen - cryptKeyLen - 4], 4);

    int key;

    if (MAGIC_COMPRESS_CRYPT_START == buffer[offset] || MAGIC_CRYPT_START == buffer[offset]) {
        key = BASE_KEY ^ (0xff & length) ^ buffer[offset];
    } else {
        unsigned short seq;
        memcpy(&seq, &buffer[offset + headerLen - cryptKeyLen - 4 - 2 - 2], 2);
        char beginHour;
        memcpy(&beginHour, &buffer[offset + headerLen - cryptKeyLen - 4 - 1 - 1], 1);
        char endHour;
        memcpy(&endHour, &buffer[offset + headerLen - cryptKeyLen - 4 - 1], 1);

        key = BASE_KEY ^ (0xff & seq) ^ buffer[offset];

        if (seq != 0 && seq != 1 && lastseq != 0 && seq != (lastseq + 1)) {
            char text[128];
            snprintf(text, sizeof(text), "[F]decode_log_file.py log seq:%d-%d is missing\n", lastseq + 1, seq - 1);
            appendBuffer(outBuffer, outBufferSize, writePos, text, strlen(text));
        }

        if (seq != 0) {
            lastseq = seq;
        }
    }

    char *tmpBuffer = (char *)realloc(NULL, length);
    size_t tmpBufferSize = length;
    if (tmpBuffer == NULL) {
        fputs("Memory error\n", stderr);
        exit(2);
    }

    if (MAGIC_COMPRESS_CRYPT_START == buffer[offset] ||
        NEW_MAGIC_COMPRESS_CRYPT_START == buffer[offset]) {
        size_t i;
        for (i = 0; i < length; i++) {
            tmpBuffer[i] = key ^ buffer[offset + headerLen + i];
        }
        char *decompBuffer;
        size_t decompBufferSize;
        if (!zlibDecompress(tmpBuffer, tmpBufferSize, &decompBuffer, &decompBufferSize)) {
            fputs("Decompress error\n", stderr);
            exit(6);
        }

        free(tmpBuffer);
        tmpBuffer = decompBuffer;
        tmpBufferSize = decompBufferSize;
    } else if (NEW_MAGIC_COMPRESS_CRYPT_START1 == buffer[offset]) {
        size_t readPos = 0;
        size_t readSize = 0;
        size_t tmpBufferWritePos = 0;
        while (readPos < length) {
            uint16_t singleLogLen;
            memcpy(&singleLogLen, buffer + offset + headerLen + readPos, 2);
            appendBuffer(&tmpBuffer, &tmpBufferSize, &tmpBufferWritePos,
                         buffer + offset + headerLen + readPos + 2, singleLogLen);
            readSize += singleLogLen;
            readPos += singleLogLen + 2;
        }

        size_t i;
        for (i = 0; i < readSize; i++) {
            tmpBuffer[i] = key ^ tmpBuffer[i];
        }

        char *decompBuffer;
        size_t decompBufferSize;
        if (!zlibDecompress(tmpBuffer, tmpBufferSize, &decompBuffer, &decompBufferSize)) {
            fputs("Decompress error\n", stderr);
            exit(6);
        }

        free(tmpBuffer);
        tmpBuffer = decompBuffer;
        tmpBufferSize = decompBufferSize;
    } else if (MAGIC_SYNC_ZLIB_START == buffer[offset] ||
               MAGIC_SYNC_NO_CRYPT_ZLIB_START == buffer[offset] ||
               MAGIC_SYNC_ZSTD_START == buffer[offset] ||
               MAGIC_SYNC_NO_CRYPT_ZSTD_START == buffer[offset]) {
        memcpy(tmpBuffer, buffer + offset + headerLen, length);
    } else if (MAGIC_ASYNC_ZLIB_START == buffer[offset] || MAGIC_ASYNC_ZSTD_START == buffer[offset]) {
        memcpy(tmpBuffer, buffer + offset + headerLen, length);
        unsigned char clientPubKey[cryptKeyLen];
        memcpy(clientPubKey, buffer + offset + headerLen - cryptKeyLen, cryptKeyLen);

        unsigned char svrPriKey[32] = {0};
        if (!Hex2Buffer(PRIV_KEY, 64, svrPriKey)) {
            fputs("Get PRIV KEY error\n", stderr);
            exit(7);
        }

        unsigned char ecdhKey[32] = {0};
        if (0 == uECC_shared_secret(clientPubKey, svrPriKey, ecdhKey, uECC_secp256k1())) {
            fputs("Get ECDH key error\n", stderr);
            exit(8);
        }

        uint32_t teaKey[4];
        memcpy(teaKey, ecdhKey, sizeof(teaKey));
        uint32_t tmp[2] = {0};
        size_t cnt = length / TEA_BLOCK_LEN;

        size_t i;
        for (i = 0; i < cnt; i++) {
            memcpy(tmp, tmpBuffer + i * TEA_BLOCK_LEN, TEA_BLOCK_LEN);
            teaDecrypt(tmp, teaKey);
            memcpy(tmpBuffer + i * TEA_BLOCK_LEN, tmp, TEA_BLOCK_LEN);
        }

        char *decompBuffer;
        size_t decompBufferSize;
        if (MAGIC_ASYNC_ZLIB_START == buffer[offset]) {
            if (!zlibDecompress(tmpBuffer, tmpBufferSize, &decompBuffer, &decompBufferSize)) {
                fputs("Decompress error\n", stderr);
                exit(6);
            }
        } else if (MAGIC_ASYNC_ZSTD_START == buffer[offset]) {
            if (!zstdDecompress(tmpBuffer, tmpBufferSize, &decompBuffer, &decompBufferSize)) {
                fputs("Decompress error\n", stderr);
                exit(6);
            }
        }

        free(tmpBuffer);
        tmpBuffer = decompBuffer;
        tmpBufferSize = decompBufferSize;
    } else if (MAGIC_ASYNC_NO_CRYPT_ZLIB_START == buffer[offset] || MAGIC_ASYNC_NO_CRYPT_ZSTD_START == buffer[offset]) {
        memcpy(tmpBuffer, buffer + offset + headerLen, length);
        char *decompBuffer;
        size_t decompBufferSize;
        if (MAGIC_ASYNC_NO_CRYPT_ZLIB_START == buffer[offset]) {
            if (!zlibDecompress(tmpBuffer, tmpBufferSize, &decompBuffer, &decompBufferSize)) {
                fputs("Decompress error\n", stderr);
                exit(6);
            }
        } else if (MAGIC_ASYNC_NO_CRYPT_ZSTD_START == buffer[offset]) {
            if (!zstdDecompress(tmpBuffer, tmpBufferSize, &decompBuffer, &decompBufferSize)) {
                fputs("Decompress error\n", stderr);
                exit(6);
            }
        }

        free(tmpBuffer);
        tmpBuffer = decompBuffer;
        tmpBufferSize = decompBufferSize;
    } else {
        size_t i;
        for (i = 0; i < length; i++) {
            tmpBuffer[i] = key ^ buffer[offset + headerLen + i];
        }
    }

    appendBuffer(outBuffer, outBufferSize, writePos, tmpBuffer, tmpBufferSize);
    free(tmpBuffer);

    return offset + headerLen + length + 1;
}

void parseFile(const char *path, const char *outPath) {
    FILE *file;
    size_t bufferSize;
    char *buffer;
    size_t result;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "File error code %d\n", errno);
        exit(1);
    }

    fseek(file, 0, SEEK_END);
    bufferSize = (size_t)ftell(file);
    rewind(file);

    buffer = (char *)malloc(sizeof(char) * bufferSize);
    if (buffer == NULL) {
        fputs("Memory error\n", stderr);
        exit(2);
    }

    result = fread(buffer, 1, bufferSize, file);
    if (result != bufferSize) {
        fputs("Reading error\n", stderr);
        exit(3);
    }
    fclose(file);

    size_t startPos;
    startPos = getLogStartPos(buffer, bufferSize, 2);
    if (-1 == startPos) {
        return;
    }

    size_t outBufferSize = bufferSize * 6;
    char *outBuffer = (char *)realloc(NULL, outBufferSize);
    size_t writePos = 0;
    while (1) {
        startPos = decodeBuffer(buffer, bufferSize, startPos, &outBuffer, &outBufferSize, &writePos);
        if (-1 == startPos) {
            break;
        }
    }

    FILE *outFile = fopen(outPath, "wb");
    outBuffer[writePos] = '\0';
    fwrite(outBuffer, sizeof(char), writePos, outFile);
    fclose(outFile);
    free(buffer);
    free(outBuffer);
}

void parseDir(const char *path, const char *outDir) {
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(path)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (strlen(ent->d_name) > 5 && strcmp(ent->d_name + strlen(ent->d_name) - 5, ".xlog") == 0) {
                char inPath[256] = {0};
                char outPath[256] = {0};

                snprintf(inPath, sizeof(inPath), "%s/%s", path, ent->d_name);

                char outFileName[200] = {0};
                snprintf(outFileName, sizeof(outFileName), "%s.log", ent->d_name);

                const char *paths[3];
                paths[0] = outDir;
                paths[1] = outFileName;
                paths[2] = NULL;
                cwk_path_join_multiple(paths, outPath, sizeof(outPath));

                //                if (outDir == NULL) {
                //                    snprintf(outPath, sizeof(outPath), "%s/%s.log", path, ent->d_name);
                //                } else {
                //                    int len = sizeof(outDir);
                //                    strcpy(outPath, outDir);
                //                    snprintf(outPath + len, sizeof(outPath), <#const char *restrict __format, ... #>)
                //                        snprintf(outPath + len, sizeof(outPath), "%s/%s.log", path, ent->d_name);
                //                }
                lastseq = 0;
                parseFile(inPath, outPath);
            }
        }
        closedir(dir);
    } else {
        fputs("opendir failed\n", stderr);
        exit(1);
    }
}

int main(int argc, char *argv[]) {

    const char *inPath = NULL;
    const char *outPath = NULL;

    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_GROUP("Basic options"),
        OPT_STRING('i', "in", &inPath, "待解密文件 可以是文件目录, 也可以是文件"),
        OPT_STRING('o', "out", &outPath, "如果输入是目录, 则输出也必须是目录, 如果输入是单个文件, 则可以是指定输出文件或者输出目录"),
        OPT_STRING('k', "key", &PRIV_KEY, "私钥"),
        //        OPT_STRING(NULL, "public", &PUB_KEY, "公钥"),
        OPT_END(),
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usage, 0);
    const char *description = NULL;  //"\nA brief description of what the program does and how it works.";
    const char *epilog = NULL;       //"\nAdditional description of the program after the description of the arguments.";
    argparse_describe(&argparse,
                      description,
                      epilog);

    argc = argparse_parse(&argparse, argc, (const char **)argv);

    if (inPath == NULL || outPath == NULL) {
        argparse_usage(&argparse);
        return EXIT_FAILURE;
    }

    if (PRIV_KEY == NULL) {
        argparse_usage(&argparse);
        return EXIT_FAILURE;
    }

    cwk_path_set_style(CWK_STYLE_UNIX);

    struct stat in_path_stat;
    stat(inPath, &in_path_stat);

    if (S_ISREG(in_path_stat.st_mode)) {
        if (strcmp(inPath, outPath) == 0) {
            printf("error: 输出文件和输入文件一致\n");
            return EXIT_FAILURE;
        }
        if (outPath != NULL) {
            struct stat out_path_stat;
            stat(outPath, &out_path_stat);
            if (S_ISDIR(out_path_stat.st_mode)) {
                char *basename = NULL;
                size_t len = 0;
                cwk_path_get_basename(inPath, (const char **)&basename, &len);
                if (basename != NULL) {
                    char fixOutPath[256] = {0};
                    const char *paths[3];
                    paths[0] = outPath;
                    paths[1] = basename;
                    paths[2] = NULL;
                    cwk_path_join_multiple(paths, fixOutPath, sizeof(fixOutPath));
                    outPath = fixOutPath;
                    char *ext_buf = NULL;
                    size_t ext_len = 0;
                    cwk_path_get_extension(fixOutPath, (const char **)&ext_buf, &ext_len);
                    if (ext_len > 0) {
                        char changeExtBuf[256 + 5] = {0};
                        size_t len = ext_len + 5;
                        char *new_ext = (char *)alloca(len);
                        if (new_ext == NULL) {
                            printf("alloca error: %d\n", errno);
                            return EXIT_FAILURE;
                        }
                        memset(new_ext, 0, len);
                        strcpy(new_ext, ext_buf);
                        strcpy(new_ext + ext_len, ".log");
                        cwk_path_change_extension(fixOutPath, new_ext, changeExtBuf, sizeof(changeExtBuf));
                        outPath = changeExtBuf;
                    }
                }
            }
        }
        parseFile(inPath, outPath);
    } else if (S_ISDIR(in_path_stat.st_mode)) {
        if (outPath != NULL) {
            struct stat out_path_stat;
            stat(outPath, &out_path_stat);

            if (!S_ISDIR(out_path_stat.st_mode)) {
                argparse_usage(&argparse);
                return EXIT_FAILURE;
            }

            if (access(outPath, F_OK) != 0) {
                if (mkdir(outPath, (S_IRWXU | S_IRWXG | S_IRWXO)) != 0) {
                    printf("%s 目录创建失败 %d", outPath, errno);
                    return EXIT_FAILURE;
                }
            }
        } else {
            outPath = inPath;
        }
        parseDir(inPath, outPath);
    } else {
        argparse_usage(&argparse);
        return EXIT_FAILURE;
    }
    //        if (argc == 2) {
    //            char *path = argv[1];
    //            struct stat path_stat;
    //            stat(path, &path_stat);
    //
    //            if (S_ISREG(path_stat.st_mode)) {
    //                char outPath[260] = {0};
    //                snprintf(outPath, sizeof(outPath), "%s.log", path);
    //                parseFile(path, outPath);
    //            } else if (S_ISDIR(path_stat.st_mode)) {
    //                parseDir(path);
    //            } else {
    //                fputs("openfile failed", stderr);
    //                return 1;
    //            }
    //        } else if (argc == 3) {
    //            char *inPath = argv[1];
    //            char *outPath = argv[2];
    //            parseFile(inPath, outPath);
    //        } else {
    //            parseDir(".");
    //        }
    return 0;
}
