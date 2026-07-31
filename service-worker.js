const CACHE="rumcajs-media-v4";
const CORE=["./","./index.html","./style.css?v=4","./app.js?v=4","./media.json","./manifest.webmanifest","./covers/default-cover.svg","./assets/icons/icon-192.png","./assets/icons/icon-512.png"];
self.addEventListener("install",e=>{e.waitUntil(caches.open(CACHE).then(c=>c.addAll(CORE)));self.skipWaiting()});
self.addEventListener("activate",e=>{e.waitUntil(caches.keys().then(k=>Promise.all(k.filter(x=>x!==CACHE).map(x=>caches.delete(x)))));self.clients.claim()});
self.addEventListener("fetch",e=>{if(e.request.method!=="GET")return;const u=new URL(e.request.url);if(u.origin!==location.origin)return;if(/\.(mp3|m4a|aac|wav|ogg|mp4|webm|mov)$/i.test(u.pathname)){e.respondWith(fetch(e.request).catch(()=>caches.match(e.request)));return}e.respondWith(fetch(e.request).then(r=>{const c=r.clone();caches.open(CACHE).then(x=>x.put(e.request,c));return r}).catch(()=>caches.match(e.request).then(r=>r||caches.match("./index.html"))))});
