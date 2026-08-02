const els={
  tabs:document.querySelector('#categoryTabs'),grid:document.querySelector('#mediaGrid'),template:document.querySelector('#cardTemplate'),
  search:document.querySelector('#searchInput'),sort:document.querySelector('#sortSelect'),back:document.querySelector('#backToFolders'),
  sectionTitle:document.querySelector('#sectionTitle'),count:document.querySelector('#itemCount'),empty:document.querySelector('#emptyState'),error:document.querySelector('#errorState'),share:document.querySelector('#shareButton'),
  siteTitle:document.querySelector('#siteTitle'),siteSubtitle:document.querySelector('#siteSubtitle'),footer:document.querySelector('#footerText'),
  panel:document.querySelector('#playerPanel'),cover:document.querySelector('#playerCover'),playerTitle:document.querySelector('#playerTitle'),playerMeta:document.querySelector('#playerMeta'),audio:document.querySelector('#audioPlayer'),video:document.querySelector('#videoPlayer'),
  proposalButton:document.querySelector('#proposalButton'),proposalPanel:document.querySelector('#proposalPanel'),closeProposal:document.querySelector('#closeProposalButton'),proposalLoginBox:document.querySelector('#proposalLoginBox'),proposalLogin:document.querySelector('#proposalLoginButton'),proposalForm:document.querySelector('#proposalForm'),proposalName:document.querySelector('#proposalName'),proposalEmail:document.querySelector('#proposalEmail'),proposalTitle:document.querySelector('#proposalTitle'),proposalAuthor:document.querySelector('#proposalAuthor'),proposalCategory:document.querySelector('#proposalCategory'),proposalDescription:document.querySelector('#proposalDescription'),proposalFile:document.querySelector('#proposalFile'),proposalRights:document.querySelector('#proposalRights'),proposalPublish:document.querySelector('#proposalPublish'),proposalRules:document.querySelector('#proposalRules'),proposalSubmit:document.querySelector('#proposalSubmit'),proposalProgress:document.querySelector('#proposalProgress'),proposalMessage:document.querySelector('#proposalMessage'),
  adminButton:document.querySelector('#adminButton'),adminPanel:document.querySelector('#adminPanel'),closeAdmin:document.querySelector('#closeAdminButton'),adminLoginBox:document.querySelector('#adminLoginBox'),adminLogin:document.querySelector('#adminLoginButton'),uploadForm:document.querySelector('#uploadForm'),uploadTitle:document.querySelector('#uploadTitle'),uploadSubtitle:document.querySelector('#uploadSubtitle'),uploadCategory:document.querySelector('#uploadCategory'),uploadDescription:document.querySelector('#uploadDescription'),uploadFile:document.querySelector('#uploadFile'),uploadSubmit:document.querySelector('#uploadSubmit'),uploadProgress:document.querySelector('#uploadProgress'),adminMessage:document.querySelector('#adminMessage')
};

const state={items:[],staticItems:[],remoteItems:[],category:'all',query:'',sort:'newest',isAdmin:false,user:null};
const labels={all:'Wszystko',ai:'Muzyka AI',mine:'Moja muzyka',other:'Inna muzyka',podcast:'Nagrania / podcasty',effects:'Dźwięki / efekty',music:'Muzyka',video:'Filmy',photo:'Zdjęcia',document:'Dokumenty',download:'Pliki'};
const icons={all:'▦',ai:'✦',mine:'♪',other:'♫',podcast:'◉',effects:'⚡',music:'♪',video:'▶',photo:'▣',document:'▤',download:'↓'};
let auth=null,db=null,storage=null;

function txt(v,f=''){return typeof v==='string'&&v.trim()?v.trim():f}
function categoryKey(item){return item.category||item.type||'other'}
function categoryLabel(c){return labels[c]||c}
function timestampValue(v){if(!v)return 0;if(typeof v.toMillis==='function')return v.toMillis();if(v.seconds)return v.seconds*1000;const n=Date.parse(v);return Number.isNaN(n)?0:n}
function rebuildItems(){state.items=[...state.staticItems,...state.remoteItems];renderFolders();renderItems()}

function renderFolders(){
  const preferred=['all','ai','mine','other','podcast','effects'];
  const available=[...new Set(state.items.map(categoryKey).filter(Boolean))];
  const cats=[...preferred.filter(c=>c==='all'||available.includes(c)),...available.filter(c=>!preferred.includes(c))];
  els.tabs.replaceChildren();
  cats.forEach(cat=>{
    const count=cat==='all'?state.items.length:state.items.filter(i=>categoryKey(i)===cat).length;
    const b=document.createElement('button');b.type='button';b.className='folder'+(state.category===cat?' active':'');
    const icon=document.createElement('span');icon.className='folder-icon';icon.textContent=icons[cat]||'▣';
    const text=document.createElement('span');const title=document.createElement('span');title.className='folder-title';title.textContent=categoryLabel(cat);const sub=document.createElement('span');sub.className='folder-count';sub.textContent=`${count} ${count===1?'pozycja':'pozycji'}`;text.append(title,sub);b.append(icon,text);
    b.onclick=()=>{state.category=cat;renderFolders();renderItems();document.querySelector('#sectionTitle')?.scrollIntoView({behavior:'smooth',block:'center'})};
    els.tabs.appendChild(b);
  });
}

function filtered(){
  let items=state.items.filter(i=>{
    const cat=state.category==='all'||categoryKey(i)===state.category;
    const hay=[i.title,i.subtitle,i.description,i.type,i.category].filter(Boolean).join(' ').toLocaleLowerCase('pl');
    return cat&&hay.includes(state.query);
  });
  items=[...items].sort((a,b)=>{
    if(state.sort==='title')return txt(a.title).localeCompare(txt(b.title),'pl',{sensitivity:'base'});
    if(state.sort==='artist')return txt(a.subtitle).localeCompare(txt(b.subtitle),'pl',{sensitivity:'base'});
    return timestampValue(b.createdAt)-timestampValue(a.createdAt);
  });
  return items;
}

function addButton(container,label,href,extraClass=''){
  const a=document.createElement('a');a.className='button '+extraClass;a.textContent=label;a.href=href;a.target='_blank';a.rel='noopener';container.appendChild(a);
}

function renderItems(){
  const items=filtered();els.grid.replaceChildren();els.count.textContent=`${items.length} ${items.length===1?'pozycja':'pozycji'}`;els.empty.hidden=items.length!==0;els.sectionTitle.textContent=categoryLabel(state.category);els.back.hidden=state.category==='all';
  items.forEach(item=>{
    const n=els.template.content.cloneNode(true);const row=n.querySelector('.track-row');const main=n.querySelector('.track-main');const details=n.querySelector('.track-details');
    n.querySelector('.badge').textContent=categoryLabel(categoryKey(item));n.querySelector('.card-title').textContent=txt(item.title,'Bez tytułu');n.querySelector('.card-subtitle').textContent=txt(item.subtitle,'—');
    const d=n.querySelector('.card-description');d.textContent=txt(item.description,'Brak dodatkowego opisu.');const actions=n.querySelector('.card-actions');const file=txt(item.file);
    main.onclick=()=>{const open=details.hidden;details.hidden=!open;row.classList.toggle('open',open);main.setAttribute('aria-expanded',String(open))};
    if(item.type==='music'&&file){const b=document.createElement('button');b.className='button';b.type='button';b.textContent='▶ Odtwórz';b.onclick=()=>playMedia(item,'audio');actions.appendChild(b)}
    else if(item.type==='video'&&file){const b=document.createElement('button');b.className='button';b.type='button';b.textContent='▶ Odtwórz';b.onclick=()=>playMedia(item,'video');actions.appendChild(b)}
    else if(item.type==='photo'&&file){addButton(actions,'Otwórz zdjęcie',file)}else if(file){addButton(actions,'Otwórz',file)}
    if(file&&item.allowDownload!==false){const a=document.createElement('a');a.className='button ghost';a.textContent='Pobierz';a.href=file;a.download='';actions.appendChild(a)}
    els.grid.appendChild(n);
  });
}

function playMedia(item,kind){
  els.panel.hidden=false;els.cover.src=txt(item.cover,'./covers/default-cover.svg');els.playerTitle.textContent=txt(item.title,'Bez tytułu');els.playerMeta.textContent=[txt(item.subtitle),categoryLabel(categoryKey(item))].filter(Boolean).join(' · ');els.audio.pause();els.video.pause();els.audio.hidden=kind!=='audio';els.video.hidden=kind!=='video';const p=kind==='audio'?els.audio:els.video;p.src=item.file;p.play().catch(()=>{});els.panel.scrollIntoView({behavior:'smooth',block:'nearest'});
}

function slugify(value){return txt(value,'utwor').toLocaleLowerCase('pl').normalize('NFD').replace(/[\u0300-\u036f]/g,'').replace(/[^a-z0-9]+/g,'-').replace(/^-|-$/g,'').slice(0,60)||'utwor'}
function titleFromFilename(name){return name.replace(/\.[^.]+$/,'').replace(/[-_]+/g,' ').replace(/\s+/g,' ').trim().replace(/\b\w/g,c=>c.toLocaleUpperCase('pl'))}
function setAdminMessage(message,isError=false){els.adminMessage.textContent=message;els.adminMessage.classList.toggle('error',isError)}
function setProposalMessage(message,isError=false){els.proposalMessage.textContent=message;els.proposalMessage.classList.toggle('error',isError)}
function openPanel(panel){els.adminPanel.hidden=true;els.proposalPanel.hidden=true;panel.hidden=false;panel.scrollIntoView({behavior:'smooth',block:'start'})}

async function loadRemoteItems(){if(!db)return;try{const snap=await db.collection('mediaItems').orderBy('createdAt','desc').get();state.remoteItems=snap.docs.map(d=>({id:d.id,...d.data()}));rebuildItems()}catch(e){console.warn('Nie udało się wczytać Firestore',e)}}

async function checkAdmin(user){
  state.isAdmin=false;els.uploadForm.hidden=true;
  if(!user){els.adminLoginBox.hidden=false;setAdminMessage('Zaloguj się kontem administratora Dashboardu.');return}
  try{const [u,a]=await Promise.all([db.collection('users').doc(user.uid).get(),db.collection('admins').doc(user.uid).get()]);const active=u.exists&&u.data().active===true;const admin=a.exists&&a.data().role==='admin';state.isAdmin=active&&admin;els.adminLoginBox.hidden=state.isAdmin;els.uploadForm.hidden=!state.isAdmin;const account=user.email||user.displayName||user.uid;setAdminMessage(state.isAdmin?`Zalogowano jako administrator: ${account}`:`Zalogowane konto nie ma praw administratora: ${account}.`,!state.isAdmin)}catch(e){setAdminMessage(`Nie udało się sprawdzić uprawnień: ${e.message||e}`,true)}
}

function updateProposalAccess(user){state.user=user||null;els.proposalLoginBox.hidden=!!user;els.proposalForm.hidden=!user;if(user){if(!els.proposalName.value)els.proposalName.value=user.displayName||'';if(!els.proposalEmail.value)els.proposalEmail.value=user.email||'';setProposalMessage(`Zalogowano jako ${user.email||user.displayName||'użytkownik Google'}. Materiał trafi wyłącznie do kolejki administratora.`)}else setProposalMessage('Zaloguj się, aby wysłać propozycję.')}

async function googleLogin(mode){if(!auth)return(mode==='proposal'?setProposalMessage:setAdminMessage)('Firebase nie jest gotowy.',true);try{const provider=new firebase.auth.GoogleAuthProvider();provider.setCustomParameters({prompt:'select_account'});(mode==='proposal'?setProposalMessage:setAdminMessage)('Otwieranie logowania Google…');await auth.signInWithPopup(provider)}catch(e){(mode==='proposal'?setProposalMessage:setAdminMessage)(`Logowanie nie powiodło się: ${e.message||e}`,true)}}
async function loginAdmin(){return googleLogin('admin')}
async function loginProposal(){return googleLogin('proposal')}

async function submitProposal(event){
  event.preventDefault();const user=auth?.currentUser;if(!user)return setProposalMessage('Najpierw zaloguj się kontem Google.',true);const file=els.proposalFile.files?.[0];if(!file)return setProposalMessage('Wybierz plik audio.',true);if(file.size>100*1024*1024)return setProposalMessage('Plik jest większy niż 100 MB.',true);if(!els.proposalRights.checked||!els.proposalPublish.checked||!els.proposalRules.checked)return setProposalMessage('Zaznacz wszystkie wymagane oświadczenia.',true);
  const allowed=['audio/mpeg','audio/mp3','audio/mp4','audio/x-m4a','audio/wav','audio/x-wav','audio/flac','audio/x-flac'];const ext=(file.name.split('.').pop()||'').toLowerCase();if(!allowed.includes(file.type)&&!['mp3','m4a','wav','flac'].includes(ext))return setProposalMessage('Nieobsługiwany format pliku.',true);
  els.proposalSubmit.disabled=true;els.proposalProgress.hidden=false;els.proposalProgress.value=0;setProposalMessage('Przesyłanie propozycji…');const proposalId=db.collection('mediaProposals').doc().id;const safeName=`${Date.now()}-${slugify(els.proposalTitle.value)}.${ext||'mp3'}`;const ref=storage.ref().child(`media-proposals/${user.uid}/${proposalId}/${safeName}`);const task=ref.put(file,{contentType:file.type||'application/octet-stream',customMetadata:{proposalId,ownerUid:user.uid}});
  task.on('state_changed',s=>{els.proposalProgress.value=Math.round((s.bytesTransferred/s.totalBytes)*100)},e=>{els.proposalSubmit.disabled=false;els.proposalProgress.hidden=true;setProposalMessage(`Nie udało się przesłać pliku: ${e.message||e}`,true)},async()=>{try{const fileUrl=await task.snapshot.ref.getDownloadURL();await db.collection('mediaProposals').doc(proposalId).set({status:'pending',title:txt(els.proposalTitle.value),author:txt(els.proposalAuthor.value),category:els.proposalCategory.value,description:txt(els.proposalDescription.value),submitterName:txt(els.proposalName.value),contactEmail:txt(els.proposalEmail.value),accountEmail:user.email||'',submitterUid:user.uid,fileUrl,storagePath:task.snapshot.ref.fullPath,originalFilename:file.name,fileSize:file.size,contentType:file.type||'',rightsConfirmed:true,publicationConsent:true,rulesAccepted:true,source:'rumcajs-media-center',createdAt:firebase.firestore.FieldValue.serverTimestamp()});els.proposalForm.reset();els.proposalName.value=user.displayName||'';els.proposalEmail.value=user.email||'';els.proposalProgress.hidden=true;setProposalMessage(`Dziękujemy. Propozycja została zapisana pod numerem ${proposalId.slice(0,8).toUpperCase()} i czeka na decyzję administratora.`)}catch(e){try{await task.snapshot.ref.delete()}catch(_){}setProposalMessage(`Plik przesłano, ale nie udało się zapisać zgłoszenia: ${e.message||e}`,true)}finally{els.proposalSubmit.disabled=false}});
}

async function uploadTrack(event){
  event.preventDefault();if(!state.isAdmin)return setAdminMessage(`Brak uprawnień administratora dla konta ${auth?.currentUser?.email||'nieznanego'}.`,true);const file=els.uploadFile.files?.[0];if(!file)return setAdminMessage('Wybierz plik MP3.',true);if(file.size>100*1024*1024)return setAdminMessage('Plik jest większy niż 100 MB.',true);
  els.uploadSubmit.disabled=true;els.uploadProgress.hidden=false;els.uploadProgress.value=0;setAdminMessage('Przesyłanie pliku…');const category=els.uploadCategory.value;const filename=`${Date.now()}-${slugify(els.uploadTitle.value||titleFromFilename(file.name))}.mp3`;const ref=storage.ref().child(`media/music/${category}/${filename}`);const task=ref.put(file,{contentType:file.type||'audio/mpeg'});
  task.on('state_changed',s=>{els.uploadProgress.value=Math.round((s.bytesTransferred/s.totalBytes)*100)},e=>{els.uploadSubmit.disabled=false;setAdminMessage(`Błąd przesyłania: ${e.message||e}`,true)},async()=>{try{const url=await task.snapshot.ref.getDownloadURL();await db.collection('mediaItems').add({type:'music',category,title:txt(els.uploadTitle.value,titleFromFilename(file.name)),subtitle:txt(els.uploadSubtitle.value,'Rumcajs'),description:txt(els.uploadDescription.value),file:url,cover:'./covers/default-cover.svg',allowDownload:false,originalFilename:file.name,fileSize:file.size,createdAt:firebase.firestore.FieldValue.serverTimestamp(),ownerUid:auth.currentUser.uid});els.uploadForm.reset();els.uploadProgress.hidden=true;state.category=category;setAdminMessage(`Utwór został dodany do katalogu „${categoryLabel(category)}”.`);await loadRemoteItems()}catch(e){setAdminMessage(`Nie udało się zapisać utworu: ${e.message||e}`,true)}finally{els.uploadSubmit.disabled=false}});
}

els.search.addEventListener('input',()=>{state.query=els.search.value.trim().toLocaleLowerCase('pl');renderItems()});
els.sort.addEventListener('change',()=>{state.sort=els.sort.value;renderItems()});
els.back.addEventListener('click',()=>{state.category='all';renderFolders();renderItems();document.querySelector('.library-head')?.scrollIntoView({behavior:'smooth',block:'start'})});
els.share.addEventListener('click',async()=>{try{if(navigator.share)await navigator.share({title:document.title,url:location.href});else{await navigator.clipboard.writeText(location.href);const t=els.share.textContent;els.share.textContent='Link skopiowany';setTimeout(()=>els.share.textContent=t,1500)}}catch(e){if(e.name!=='AbortError')console.error(e)}});
els.proposalButton.addEventListener('click',()=>openPanel(els.proposalPanel));els.closeProposal.addEventListener('click',()=>{els.proposalPanel.hidden=true});els.proposalLogin.addEventListener('click',loginProposal);els.proposalFile.addEventListener('change',()=>{const file=els.proposalFile.files?.[0];if(file&&!els.proposalTitle.value.trim())els.proposalTitle.value=titleFromFilename(file.name)});els.proposalForm.addEventListener('submit',submitProposal);
els.adminButton.addEventListener('click',()=>openPanel(els.adminPanel));els.closeAdmin.addEventListener('click',()=>{els.adminPanel.hidden=true});els.adminLogin.addEventListener('click',loginAdmin);
els.uploadFile.addEventListener('change',()=>{const file=els.uploadFile.files?.[0];if(!file)return;if(!els.uploadTitle.value.trim())els.uploadTitle.value=titleFromFilename(file.name);if(!els.uploadSubtitle.value.trim())els.uploadSubtitle.value=els.uploadCategory.value==='ai'?'Rumcajs AI':'Rumcajs';if(!els.uploadDescription.value.trim())els.uploadDescription.value=`Plik: ${file.name}`});
els.uploadCategory.addEventListener('change',()=>{if(!els.uploadSubtitle.value.trim()||els.uploadSubtitle.value==='Rumcajs'||els.uploadSubtitle.value==='Rumcajs AI')els.uploadSubtitle.value=els.uploadCategory.value==='ai'?'Rumcajs AI':'Rumcajs'});els.uploadForm.addEventListener('submit',uploadTrack);

async function init(){
  try{const r=await fetch('./media.json',{cache:'no-store'});if(!r.ok)throw new Error(r.status);const data=await r.json();const s=data.settings||{};els.siteTitle.textContent=txt(s.title,'Rumcajs Media Center');els.siteSubtitle.textContent=txt(s.subtitle,'Muzyka, filmy, zdjęcia i pliki w jednym miejscu');els.footer.textContent=txt(s.footer,'© 2026 Rumcajs Media Center');document.title=els.siteTitle.textContent;state.staticItems=Array.isArray(data.items)?data.items.filter(Boolean):[];rebuildItems()}catch(e){console.error(e);els.error.hidden=false}
  try{if(window.firebase&&firebase.apps.length){auth=firebase.auth();db=firebase.firestore();storage=firebase.storage();auth.onAuthStateChanged(user=>{state.user=user||null;checkAdmin(user);updateProposalAccess(user)});await loadRemoteItems()}else{console.warn('Firebase nie został zainicjalizowany.')}}catch(e){console.error('Firebase init error',e);setAdminMessage(`Firebase: ${e.message||e}`,true)}
}

if('serviceWorker'in navigator)window.addEventListener('load',()=>navigator.serviceWorker.register('./service-worker.js'));
init();
