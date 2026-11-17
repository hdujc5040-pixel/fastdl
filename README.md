# fastdl

A small multi-connection HTTP/HTTPS downloader for Windows. One C++ file, no dependencies (WinHTTP only), about 240 KB static exe.

It splits the file into chunks and downloads them over many connections at once with `Range` requests, which is much faster than a single connection on servers that throttle per connection. Progress looks like a browser's download panel.

```
File:    python-3.12.0-embed-amd64.zip
Size:    10.52 MB
Mode:    8 connections (range requests)

[##############--------]  63.4%  6.67 MB / 10.52 MB  4.81 MB/s  ETA 00:01  8 conn
```

## Usage

```
fastdl <url> [options]

  -o <file>      output file name (default: taken from the server / url)
  -t <n>         parallel connections, 1..128 (default 32)
  -c <mb>        chunk size in MB, 1..256 (default: automatic)
  -H "N: v"      extra request header (repeatable), e.g. -H "Cookie: a=b"
  -h             help
```

```
fastdl https://example.com/big.iso
fastdl https://example.com/big.iso -t 64 -o disk.iso
```

- **Resume.** Ctrl+C pauses (or kill the process); run the same command again and only the missing chunks are downloaded. State is kept in `<file>.fdl` next to the file.
- **Redirects**, `Content-Disposition` file names, percent-encoded URLs, system proxy settings and TLS 1.2/1.3 are handled by WinHTTP.
- Servers without `Range` support (or with unknown size) fall back to one connection.
- An existing file with a different size is never overwritten: the new one is saved as `name (1).ext`.
- HTTP/2 is turned off on purpose, so every worker gets its own TCP connection.

## Build

Needs Visual Studio 2017+ with the "Desktop development with C++" workload.

```
build.bat
```

Or by hand from a developer prompt:

```
cl /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE fastdl.cpp /Fe:fastdl.exe /link winhttp.lib
```

## Tests

`tests/run_tests.py` starts a local test server (`tests/range_server.py`) and checks ranged and single-connection downloads, redirects, file names, `404`, the speed-up on a throttled server and resume after a killed process. File contents are verified byte by byte.

```
python tests/run_tests.py
```

## По-русски

Многопоточный загрузчик файлов по прямой ссылке для Windows. Качает файл кусками через много соединений сразу (Range-запросы), показывает полосу прогресса, процент, размер, скорость и оставшееся время, как в браузере. Если прервать (Ctrl+C), та же команда продолжит с места остановки.

```
fastdl https://example.com/big.iso -t 64
```

## License

MIT
