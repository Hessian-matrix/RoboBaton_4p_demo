package main

import (
	"embed"
	"net/http"
)

//go:embed assets/index.html assets/app.js assets/style.css assets/i18n.js assets/dashboard.js assets/attitude-worker.js assets/attitude.js assets/recording.js
var assets embed.FS

func serveAsset(w http.ResponseWriter, r *http.Request) {
	paths := map[string]string{"/": "assets/index.html", "/app.js": "assets/app.js", "/style.css": "assets/style.css", "/dashboard.js": "assets/dashboard.js", "/attitude-worker.js": "assets/attitude-worker.js", "/attitude.js": "assets/attitude.js", "/recording.js": "assets/recording.js"}
	paths["/i18n.js"] = "assets/i18n.js"
	name, ok := paths[r.URL.Path]
	if !ok {
		jsonError(w, 404, "resource", "not found")
		return
	}
	if r.Method != "GET" {
		jsonError(w, 405, "method", "GET required")
		return
	}
	body, err := assets.ReadFile(name)
	if err != nil {
		jsonError(w, 503, "asset", "asset unavailable")
		return
	}
	types := map[string]string{"/": "text/html; charset=utf-8", "/app.js": "text/javascript; charset=utf-8", "/style.css": "text/css; charset=utf-8", "/dashboard.js": "text/javascript; charset=utf-8", "/attitude-worker.js": "text/javascript; charset=utf-8", "/attitude.js": "text/javascript; charset=utf-8", "/recording.js": "text/javascript; charset=utf-8"}
	types["/i18n.js"] = "text/javascript; charset=utf-8"
	w.Header().Set("Content-Type", types[r.URL.Path])
	_, _ = w.Write(body)
}
