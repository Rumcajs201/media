const els={
  tabs:document.querySelector("#categoryTabs"),grid:document.querySelector("#mediaGrid"),
  template:document.querySelector("#cardTemplate"),search:document.querySelector("#searchInput"),
  sectionTitle:document.querySelector("#sectionTitle"),count:document.querySelector("#itemCount"),
  empty:document.querySelector("#emptyState"),error:document.querySelector("#errorState"),
  share:document.querySelector("#shareButton"),siteTitle:document.querySelector("#siteTitle"),
  siteSubtitle:document.querySelector("#siteSubtitle"),footer:document.querySelector("#footerText"),
  panel:document.querySelector("#playerPanel"),cover:document.querySelector("#playerCover"),
  playerTitle:document.querySelector("#playerTitle"),playerMeta:document.querySelector("#playerMeta"),
  audio:document.querySelector("#audioPlayer"),video:document.querySelector("#videoPlayer")
};
const state={items:[],category:"all",query:""};
const labels={
  all:"Wszystko",
  ai:"Muzyka AI",
  moje:"Moja muzyka",
  inne:"Inna muzyka",
  music:"Muzyka",
  video:"Filmy",
  photo:"Zdjęcia",
  document:"Dokumenty",
  download:"Pliki"
};

function txt(v,f=""){return typeof v==="string"&&v.trim()?v.trim():f}
function categoryKey(item){return txt(item.category,txt(item.type,"inne"))}
function categoryLabel(c){return labels[c]||c}

function renderTabs(){
  const cats=["all",...new Set(state.items.map(categoryKey).filter(Boolean))];
  els.tabs.replaceChildren();
  cats.forEach(cat=>{
    const b=document.createElement("button");
    b.className="tab"+(state.category===cat?" active":"");
    b.textContent=categoryLabel(cat);
    b.onclick=()=>{state.category=cat;renderTabs();renderItems()};
    els.tabs.appendChild(b);
  });
}

function filtered(){
  return state.items.filter(i=>{
    const itemCategory=categoryKey(i);
    const cat=state.category==="all"||itemCategory===state.category;
    const hay=[i.title,i.subtitle,i.description,i.type,i.category,categoryLabel(itemCategory)].filter(Boolean).join(" ").toLocaleLowerCase("pl");
    return cat&&hay.includes(state.query);
  });
}

function addButton(container,label,href,extraClass=""){
  const a=document.createElement("a");a.className="button "+extraClass;a.textContent=label;a.href=href;a.target="_blank";a.rel="noopener";container.appendChild(a);
}
function renderItems(){
  const items=filtered();els.grid.replaceChildren();els.count.textContent=`${items.length} pozycji`;
  els.empty.hidden=items.length!==0;els.sectionTitle.textContent=categoryLabel(state.category);
  items.forEach(item=>{
    const n=els.template.content.cloneNode(true);
    n.querySelector(".card-cover").src=txt(item.cover,"./covers/default-cover.svg");
    n.querySelector(".card-cover").alt=`Okładka: ${txt(item.title,"materiał")}`;
    n.querySelector(".badge").textContent=categoryLabel(categoryKey(item));
    n.querySelector(".card-title").textContent=txt(item.title,"Bez tytułu");
    n.querySelector(".card-subtitle").textContent=txt(item.subtitle);
    const d=n.querySelector(".card-description");d.textContent=txt(item.description);d.hidden=!d.textContent;
    const actions=n.querySelector(".card-actions");
    const file=txt(item.file);
    if(item.type==="music"&&file){
      const b=document.createElement("button");b.className="button";b.textContent="▶ Odtwórz";b.onclick=()=>playMedia(item,"audio");actions.appendChild(b);
    }else if(item.type==="video"&&file){
      const b=document.createElement("button");b.className="button";b.textContent="▶ Odtwórz";b.onclick=()=>playMedia(item,"video");actions.appendChild(b);
    }else if(item.type==="photo"&&file){addButton(actions,"Otwórz zdjęcie",file)}
    else if(file){addButton(actions,"Otwórz",file)}
    if(file&&item.allowDownload!==false){const a=document.createElement("a");a.className="button ghost";a.textContent="Pobierz";a.href=file;a.download="";actions.appendChild(a)}
    els.grid.appendChild(n);
  });
}
function playMedia(item,kind){
  els.panel.hidden=false;els.cover.src=txt(item.cover,"./covers/default-cover.svg");
  els.playerTitle.textContent=txt(item.title,"Bez tytułu");els.playerMeta.textContent=txt(item.subtitle);
  els.audio.pause();els.video.pause();els.audio.hidden=kind!=="audio";els.video.hidden=kind!=="video";
  const p=kind==="audio"?els.audio:els.video;p.src=item.file;p.play().catch(()=>{});
  els.panel.scrollIntoView({behavior:"smooth",block:"nearest"});
}
els.search.addEventListener("input",()=>{state.query=els.search.value.trim().toLocaleLowerCase("pl");renderItems()});
els.share.addEventListener("click",async()=>{try{if(navigator.share)await navigator.share({title:document.title,url:location.href});else{await navigator.clipboard.writeText(location.href);const t=els.share.textContent;els.share.textContent="Link skopiowany";setTimeout(()=>els.share.textContent=t,1500)}}catch(e){if(e.name!=="AbortError")console.error(e)}});

async function init(){
  try{
    const r=await fetch("./media.json",{cache:"no-store"});if(!r.ok)throw new Error(r.status);
    const data=await r.json();const s=data.settings||{};
    els.siteTitle.textContent=txt(s.title,"Rumcajs Media Center");els.siteSubtitle.textContent=txt(s.subtitle,"Muzyka, filmy, zdjęcia i pliki w jednym miejscu");
    els.footer.textContent=txt(s.footer,"© 2026 Rumcajs Media Center");document.title=els.siteTitle.textContent;
    state.items=Array.isArray(data.items)?data.items.filter(Boolean):[];renderTabs();renderItems();
  }catch(e){console.error(e);els.error.hidden=false}
}
if("serviceWorker"in navigator)window.addEventListener("load",()=>navigator.serviceWorker.register("./service-worker.js"));
init();
