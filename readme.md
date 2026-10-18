Minimalist and fast HTTP2 Client based on libcurl for [GBFAL](https://github.com/MizaGBF/GBFAL).  

Compile with:
```console
cmake
make
```
and drop the resulting `.py`, `.dll` and `.so` files in the build folder into the GBFAL folder.
Make sure your libcurl library is compiled with GZIP support.

Usage:
```python
# initialization
http2.init(MAX_REQUEST, BASE_USER_AGENT)

# HEAD request
response = await http2.queue_request(url, "HEAD")
print(response.status)
# NOTE: for HEAD requests, the body will contain the value
# of the content-length header or an empty bytes
print(response.body)

# GET request
response = await http2.queue_request(url, "GET")
print(response.status)
print(response.body)

# exit
http2.close()
```

Consider this software highly experimental.