# xlog_decode
微信Xlog 加密日志解密

#### 生成 `Xcode` 工程

```shell
mkdir build_xcode
cd build_xcode
cmake xxx/xlog_decode -G "Xcode"  -DCMAKE_OSX_ARCHITECTURES:STRING="arm64;x86_64" -DCMAKE_CONFIGURATION_TYPES="Debug;Release"
```

#### 编译

```shell
mkdir build_out
cd build_out
cmake xxx/xlog_decode -DCMAKE_OSX_ARCHITECTURES:STRING="arm64;x86_64"
make
```

#### Usage
```shell
Usage: xlog_decode [options] [[--] args]
   or: xlog_decode [options]

    -h, --help        show this help message and exit

Basic options
    -i, --in=<str>    待解密文件 可以是文件目录, 也可以是文件
    -o, --out=<str>   解密后输出目录
    -k, --key=<str>   私钥
```
