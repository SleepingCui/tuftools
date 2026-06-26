import requests
import time

BASE_URL = "https://api.tuforums.com"
PROXIES = None
count = 0
api_time = 0

def set_proxies(proxy_url: str):
    global PROXIES
    if proxy_url:
        PROXIES = {
            "http": proxy_url,
            "https": proxy_url
        }

def fetchapi(url):
    global count
    global api_time
    global PROXIES

    count += 1
    t0 = time.perf_counter()
    r = requests.get(url, timeout=30, proxies=PROXIES)
    api_time += time.perf_counter() - t0
    r.raise_for_status()
    return r.json()

def stats():
    global count, api_time
    print(f"used {count} requests, {api_time * 1000:.2f} ms")
    print("\n"+ "=" * 70)
    count = 0
    api_time = 0