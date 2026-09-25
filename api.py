import threading
import time

import requests

BASE_URL = "https://api.tuforums.com"
PROXIES = None

# 计数器会被排名查询的线程池并发访问，+= 不是原子操作，必须加锁，
# 否则并行拉取时会丢计数和耗时。
_stats_lock = threading.Lock()
_count = 0
_api_time = 0.0
#: 本轮统计中第一个请求发出的时刻，用来算墙钟耗时。
_first_request_at: float | None = None


class ApiError(Exception):
    """网络或 API 层错误，由 UI 层转成友好提示，不再向用户抛 traceback。"""


def set_proxies(proxy_url: str):
    global PROXIES
    if proxy_url:
        PROXIES = {
            "http": proxy_url,
            "https": proxy_url
        }


def fetchapi(url):
    global PROXIES, _count, _api_time, _first_request_at

    t0 = time.perf_counter()
    try:
        r = requests.get(url, timeout=30, proxies=PROXIES)
        r.raise_for_status()
        data = r.json()
    except requests.RequestException as e:
        raise ApiError(f"请求失败：{e}") from e
    except ValueError as e:
        raise ApiError(f"响应不是合法 JSON：{e}") from e
    finally:
        # 失败的请求同样计入，这样统计反映的是真实尝试次数。
        with _stats_lock:
            if _first_request_at is None:
                _first_request_at = t0
            _count += 1
            _api_time += time.perf_counter() - t0

    return data


def take_stats() -> tuple[int, float, float]:
    """取出并清零自上次调用以来的统计。

    返回 (请求数, 请求累计耗时毫秒, 墙钟耗时毫秒)。

    排名查询是并发的，请求累计耗时是各请求耗时之和，会明显大于用户实际
    等待的墙钟时间，两个都返回，免得统计数字看起来自相矛盾。
    """
    global _count, _api_time, _first_request_at
    with _stats_lock:
        count = _count
        cumulative = _api_time * 1000
        wall = (time.perf_counter() - _first_request_at) * 1000 if _first_request_at else 0.0
        _count = 0
        _api_time = 0.0
        _first_request_at = None
    return count, cumulative, wall
