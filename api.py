import asyncio
import time

import httpx

BASE_URL = "https://api.tuforums.com"
PROXY = None
VERBOSE = False
count = 0
api_time = 0
span_start = 0.0
span_end = 0.0

def set_proxies(proxy_url: str):
    global PROXY
    if proxy_url:
        PROXY = proxy_url

def set_verbose(enabled: bool):
    global VERBOSE
    VERBOSE = enabled

def log(msg):
    if VERBOSE:
        print(msg, flush=True)

def _start_request(url):
    global count, span_start
    if count == 0:
        span_start = time.perf_counter()
        if not VERBOSE:
            print("查询中...", flush=True)
    count += 1
    if VERBOSE:
        print(f"#{count} {url}", flush=True)
    return count

def _new_client():
    return httpx.AsyncClient(proxy=PROXY, timeout=30)

async def _get(client: httpx.AsyncClient, url: str):
    global api_time, span_end
    idx = _start_request(url)
    t0 = time.perf_counter()
    try:
        r = await client.get(url)
        status, reason = r.status_code, r.reason_phrase
    except Exception as e:
        t1 = time.perf_counter()
        span_end = t1
        api_time += t1 - t0
        log(f"    <- #{idx} 请求失败 {type(e).__name__}: {e}  {(t1 - t0) * 1000:.2f} ms")
        raise

    t1 = time.perf_counter()
    span_end = t1
    api_time += t1 - t0
    log(f"    <- #{idx} {status} {reason}  {(t1 - t0) * 1000:.2f} ms")

    r.raise_for_status()
    return r.json()

async def fetchapi(url: str):
    async with _new_client() as client:
        return await _get(client, url)

async def fetchall(urls):
    async with _new_client() as client:
        return await asyncio.gather(*(_get(client, url) for url in urls), return_exceptions=True)

def fetchapi_sync(url: str):
    return asyncio.run(fetchapi(url))

def fetchall_sync(urls):
    return asyncio.run(fetchall(urls))

def stats():
    global count, api_time, span_start, span_end
    if count:
        span = (span_end - span_start) * 1000
        if VERBOSE:
            print(f"used {count} requests, span {span:.2f} ms (请求累计 {api_time * 1000:.2f} ms)")
        else:
            print(f"总耗时: {span:.2f} ms")
    print("\n"+ "=" * 70)
    count = 0
    api_time = 0
    span_start = 0.0
    span_end = 0.0
