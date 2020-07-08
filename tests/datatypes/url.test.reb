; datatypes/url.r
(url? http://www.fm.tul.cz/~ladislav/rebol)
(not url? 1)
(url! = type of http://www.fm.tul.cz/~ladislav/rebol)
; minimum; alternative literal form
(url? #[url! ""])
(strict-equal? #[url! ""] make url! 0)
(strict-equal? #[url! ""] to url! "")
("http://" = mold http://)
("http://a%2520b" = mold http://a%2520b)

; Revolt consideres URL!s to be literal/decoded forms
; https://trello.com/c/F59eH4MQ
; #2011
(
    url1: load "http://a.b.c/d?e=f%26"
    url2: load "http://a.b.c/d?e=f&"
    did all [
        not equal? url1 url2
        url1 == http://a.b.c/d?e=f%26
        url2 == http://a.b.c/d?e=f&
    ]
)

; Revolt expands the delimiters that are legal in URLs unescaped
; https://github.com/metaeducation/ren-c/issues/1046
;
(
    b: load "[http://example.com/abc{def}]"
    did all [
        (length of b) = 1
        (as text! first b) = "http://example.com/abc{def}"
    ]
)

[#2380 (
    url: decode-url http://example.com/get?q=ščř#kovtička
    did all [
        url/scheme == lit 'http  ; Note: DECODE-URL returns BLOCK! with 'http 
        url/host == "example.com"
        url/path == "/get?q=ščř"
        url/tag == "kovtička"
    ]
)(
    url: decode-url http://švéd:břéťa@example.com:8080/get?q=ščř#kovtička
    did all [
        url/scheme == lit 'http
        url/user == "švéd"
        url/pass == "břéťa"
        url/host == "example.com"
        url/port-id == 8080
        url/path == "/get?q=ščř"
        url/tag == "kovtička"
    ]
)(
    url: decode-url http://host?query
    did all [
        url/scheme == lit 'http
        url/host == "host"
        url/path == "?query"
    ]
)]
