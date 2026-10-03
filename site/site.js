// The page's script. Everything it lists comes from `SITE`, which
// scripts/site/build_site.py writes into assets/data.js from the game's own
// content: the cars, the circuits, the class blurbs.
const DATA = SITE, CLASSES = SITE.classes, ORDER = SITE.order;
// Asked for less motion: everything is shown at once and nothing pins or drifts.
const REDUCE=matchMedia("(prefers-reduced-motion: reduce)").matches;
const clamp=(v,lo,hi)=>Math.min(hi,Math.max(lo,v));
const maxPW=Math.max(...DATA.cars.map(c=>c.hp/c.mass));
const esc=s=>String(s??"").replace(/[&<>"]/g,m=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[m]));
function engine(c){
  const layout={6:"V6",8:"V8",10:"V10",12:"V12",16:"W16"}[c.cyl]||(c.cyl+"-cyl");
  const bits=[layout];
  if(c.turbo) bits.push("turbo"); else bits.push("NA");
  if(c.cyl===8&&c.cross) bits.push("crossplane");
  if(c.hybrid) bits.push("hybrid "+c.hybrid+" kW");
  bits.push((c.redline/1000).toFixed(1).replace(/\.0$/,"")+"k rpm");
  return bits.join(" · ");
}

/* cars: a block per class. Where a class fits on the screen at once the
   garage pins under the nav and scrolling steps through the classes, each
   one's cards falling into place as the last one's leave; anywhere else
   (narrow, short, less motion) the classes stack and each falls in as it is
   reached. */
const garage=document.getElementById("garage"), pin=garage.querySelector(".garage-pin"),
  rail=garage.querySelector(".garage-rail"), stage=document.getElementById("carStage"),
  tabs=document.getElementById("classTabs"), count=document.getElementById("garageCount");
const carCard=(c,k,i)=>`
      <article class="car" style="--i:${i}">
        <div class="shot${c.img?"":" none"}">${c.img?`<img src="assets/${c.img}"${c.zoom?` data-zoom="assets/${c.zoom}" tabindex="0"`:""} alt="${esc(c.name)}" loading="lazy">`:`<span>Render coming</span>`}<span class="tag">${esc(CLASSES[k].title)}</span></div>
        <div class="meta">
          <div class="brandname">${esc(c.brand)}</div>
          <h4>${esc(c.name.replace(c.brand+" ",""))}</h4>
          <dl>
            <div><dt>Power</dt><dd>${c.hp}<small>hp</small></dd></div>
            <div><dt>Mass</dt><dd>${c.mass}<small>kg</small></dd></div>
            <div><dt>hp/t</dt><dd>${Math.round(c.hp/c.mass*1000)}</dd></div>
          </dl>
          <div class="pw" title="Power-to-weight relative to the fastest car"><i style="width:${(c.hp/c.mass/maxPW*100).toFixed(1)}%"></i></div>
          <div class="eng">${engine(c)}${c.liv?` · ${c.liv} liveries`:""}</div>
        </div>
      </article>`;
stage.innerHTML=ORDER.map(k=>{
  const cars=DATA.cars.filter(c=>c.cls===k).sort((a,b)=>!!b.img-!!a.img||b.hp-a.hp);
  return `
    <div class="class-block" id="class-${k}" role="tabpanel" aria-label="${esc(CLASSES[k].title)}" style="--cols:${Math.min(cars.length,4)}">
      <div class="class-mark" aria-hidden="true">${esc(CLASSES[k].title)}</div>
      <div class="class-intro"><h3>${esc(CLASSES[k].title)}</h3><p>${esc(CLASSES[k].text)}</p></div>
      <div class="cars">${cars.map((c,i)=>carCard(c,k,i)).join("")}</div>
    </div>`;
}).join("");
tabs.innerHTML=ORDER.map((k,i)=>`<button role="tab" aria-controls="class-${k}" aria-selected="${i===0}" data-i="${i}">${esc(CLASSES[k].title)}<small>${DATA.cars.filter(c=>c.cls===k).length}</small></button>`).join("");
const blocks=[...stage.children], tabButtons=[...tabs.children], N=blocks.length;
const two=i=>String(i+1).padStart(2,"0");
count.innerHTML=`<b>01</b> / ${two(N-1)}<span class="hint">Scroll <i>↓</i></span>`;
garage.style.setProperty("--n",N);
let pinned=false, active=-1, current=-1, pinTop=0, step=1;
function highlight(i){
  if(i===current) return;
  current=i;
  tabButtons.forEach((b,j)=>b.setAttribute("aria-selected",j===i));
  count.firstChild.textContent=two(i);
  garage.classList.toggle("last",i===N-1);
}
// Pinned: class i comes on, and the one before leaves the way the page is going.
function show(i){
  if(i===active) return;
  const dir=i>active?1:-1, prev=blocks[active], next=blocks[i];
  if(prev){
    prev.style.setProperty("--dir",dir);
    prev.classList.replace("on","leaving");
    clearTimeout(prev.leave); prev.leave=setTimeout(()=>prev.classList.remove("leaving"),900);
  }
  clearTimeout(next.leave);
  next.style.setProperty("--dir",dir);
  next.classList.remove("leaving"); next.classList.add("on");
  active=i; highlight(i);
}
function layoutGarage(){
  const want=!REDUCE&&matchMedia("(min-width:1001px) and (min-height:620px)").matches;
  garage.classList.toggle("pinned",want);
  garage.classList.remove("tight");
  stage.style.height="";
  let fits=want;
  if(want){
    // The stage is as tall as the tallest class and pins only if that fits
    // under the rail: with a column per car, else with every class in four.
    const cs=getComputedStyle(pin), tallest=()=>Math.max(...blocks.map(b=>b.offsetHeight));
    const room=pin.clientHeight-parseFloat(cs.paddingTop)-parseFloat(cs.paddingBottom)
      -rail.offsetHeight-parseFloat(getComputedStyle(rail).marginBottom);
    let h=tallest();
    if(h>room){garage.classList.add("tight"); h=tallest();}
    fits=h<=room;
    if(fits) stage.style.height=h+"px"; else garage.classList.remove("pinned","tight");
  }
  if(fits!==pinned){
    pinned=fits; active=-1;
    blocks.forEach(b=>{clearTimeout(b.leave); b.classList.remove("on","leaving")});
    if(!pinned) blocks.forEach(b=>{if(b.getBoundingClientRect().top<innerHeight) b.classList.add("on")});
  }
  if(pinned){pinTop=parseFloat(getComputedStyle(pin).top)||0; step=(garage.offsetHeight-pin.offsetHeight)/N;}
}
function garageFrame(){
  if(!pinned){
    let i=0; blocks.forEach((b,j)=>{if(b.getBoundingClientRect().top<innerHeight*.5) i=j});
    highlight(i); return;
  }
  const top=garage.getBoundingClientRect().top, d=pinTop-top;
  tabButtons.forEach((b,j)=>b.style.setProperty("--p",clamp(d/step-j,0,1).toFixed(3)));
  if(active<0&&top>innerHeight*.75) return;
  show(clamp(Math.floor(d/step),0,N-1));
}
tabs.addEventListener("click",e=>{
  const b=e.target.closest("button"); if(!b) return;
  const i=+b.dataset.i;
  if(pinned) scrollTo({top:scrollY+garage.getBoundingClientRect().top-pinTop+i*step+1});
  else blocks[i].scrollIntoView({block:"start"});
});
// Stacked: each class falls in as it comes up the screen.
const blockIO=new IntersectionObserver(es=>es.forEach(e=>{if(e.isIntersecting&&!pinned) e.target.classList.add("on")}),{rootMargin:"0px 0px -15% 0px"});
blocks.forEach(b=>blockIO.observe(b));

/* tracks */
const tg=document.getElementById("trackGrid"), tf=document.getElementById("trackFilters");
const tracks=[...DATA.tracks].sort((a,b)=>a.name.localeCompare(b.name));
tg.innerHTML=tracks.map(t=>`
  <button class="trk" data-stem="${t.stem}" data-cat="${esc(t.cat)}">
    <svg viewBox="-6 -6 212 212" aria-hidden="true"><path d="${t.path}" pathLength="1"/></svg>
    <b>${esc(t.name)}</b>
    <span>${esc(t.country)} · ${(t.length/1000).toFixed(2)} km${t.dossier?' <span class="dossier">●</span>':""}</span>
  </button>`).join("");
const cats=["All",...new Set(tracks.map(t=>t.cat))];
tf.innerHTML=cats.map(c=>`<button aria-pressed="${c==="All"}" data-c="${c}">${c==="All"?"All circuits":c}</button>`).join("");
tf.onclick=e=>{
  const b=e.target.closest("button"); if(!b) return;
  [...tf.children].forEach(x=>x.setAttribute("aria-pressed",x===b));
  tg.querySelectorAll(".trk").forEach(x=>x.classList.toggle("hide",b.dataset.c!=="All"&&x.dataset.cat!==b.dataset.c));
};
const ft={bg:ftBg,path:ftPath,run:ftRun,car:ftCar,start:ftStart};
// The featured circuit's outline draws itself: on a pick, and when the section is reached.
function drawFeatured(){ft.path.classList.remove("draw"); ft.path.getBoundingClientRect(); ft.path.classList.add("draw");}
let anim=null;
function selectTrack(stem){
  const t=DATA.tracks.find(x=>x.stem===stem); if(!t) return;
  tg.querySelectorAll(".trk").forEach(x=>x.setAttribute("aria-current",x.dataset.stem===stem));
  ["bg","path","run"].forEach(k=>ft[k].setAttribute("d",t.path));
  drawFeatured();
  ft.start.setAttribute("x",t.sx-3); ft.start.setAttribute("y",t.sy-3);
  document.getElementById("ftLoc").textContent=[t.city,t.country].filter(Boolean).join(", ");
  document.getElementById("ftName").textContent=t.name;
  document.getElementById("ftLen").innerHTML=(t.length/1000).toFixed(3)+"<small>km</small>";
  document.getElementById("ftElev").innerHTML=t.elev+"<small>m</small>";
  document.getElementById("ftYear").textContent=t.year||"—";
  document.getElementById("ftAbout").textContent=t.about||"";
  document.getElementById("ftCorners").innerHTML=t.corners.length?t.corners.map(c=>`<span>${esc(c)}</span>`).join(""):`<em>${t.dossier?"Real stands, pit lane and woodland from its layout dossier.":"Dressed procedurally from its centerline."}</em>`;
  ftSvg.setAttribute("aria-label","Map of "+t.name);
  const L=ft.run.getTotalLength(); const seg=L*0.12;
  ft.run.style.strokeDasharray=`${seg} ${L}`;
  const dur=Math.max(6000,t.length*1.4); let t0=null;
  cancelAnimationFrame(anim);
  const step=ts=>{
    t0??=ts; const f=((ts-t0)%dur)/dur, d=f*L;
    ft.run.style.strokeDashoffset=-(d-seg);
    const p=ft.run.getPointAtLength(d); ft.car.setAttribute("cx",p.x); ft.car.setAttribute("cy",p.y);
    anim=requestAnimationFrame(step);
  };
  anim=requestAnimationFrame(step);
}
tg.onclick=e=>{const b=e.target.closest(".trk"); if(b){selectTrack(b.dataset.stem); if(innerWidth<1000) document.querySelector(".feature-track").scrollIntoView({behavior:"smooth",block:"start"});}};
selectTrack(DATA.tracks.some(t=>t.stem===SITE.featured)?SITE.featured:tracks[0].stem);

/* reveal: a heading (.reveal-head), a panel (.reveal) or a row of panels
   (.stagger, one after another) falls into place as it comes up the screen */
const onReveal=new Map();
const io=new IntersectionObserver(es=>es.forEach(e=>e.isIntersecting&&reveal(e.target)),{rootMargin:"0px 0px -10% 0px"});
function reveal(el){
  if(el.classList.contains("in")) return;
  el.classList.add("in"); io.unobserve(el);
  const then=onReveal.get(el); if(then) then();
}
// The stats count up to their figure.
function countUp(b){
  const t=b.firstChild; if(REDUCE||!t||t.nodeType!==3) return;
  const text=t.nodeValue, to=parseInt(text.replace(/,/g,""),10);
  if(!(to>1)) return;
  const fmt=text.includes(",")?v=>v.toLocaleString("en-US"):String, t0=performance.now(), dur=1300;
  const tick=now=>{
    const f=clamp((now-t0)/dur,0,1);
    t.nodeValue=f<1?fmt(Math.round(to*(1-Math.pow(1-f,3)))):text;
    if(f<1) requestAnimationFrame(tick);
  };
  t.nodeValue=fmt(0); requestAnimationFrame(tick);
}
onReveal.set(document.querySelector(".stats .wrap"),()=>document.querySelectorAll(".stat b").forEach((b,i)=>setTimeout(()=>countUp(b),i*90)));
onReveal.set(tg,drawFeatured);
document.querySelectorAll(".reveal,.reveal-head,.stagger").forEach(el=>{
  if(el.classList.contains("stagger")) [...el.children].forEach((c,i)=>c.style.setProperty("--i",i));
  if(REDUCE) reveal(el); else io.observe(el);
});
setTimeout(()=>document.querySelectorAll(".reveal:not(.in),.reveal-head:not(.in),.stagger:not(.in)").forEach(el=>{if(el.getBoundingClientRect().top<innerHeight)reveal(el)}),1500);

/* scroll: the nav, its progress line and the section it is in, the hero
   sinking away, the pictures that drift against the page, and the garage */
const nav=document.getElementById("nav"), readBar=document.getElementById("readBar");
const hero=document.querySelector(".hero"), heroCopy=hero.querySelector(".wrap");
const spy=[...nav.querySelectorAll("ul a")].map(a=>[a,document.querySelector(a.getAttribute("href"))]).filter(([,s])=>s);
const drifting=REDUCE?[]:[...document.querySelectorAll(".band img,.shots figure:first-child img,.cta>img")]
  .map(img=>{img.classList.add("drift"); return {img,box:img.parentElement};});
function heroFrame(y){
  if(REDUCE) return;
  const h=hero.offsetHeight, live=innerWidth>640;
  if(live&&y>h) return; // gone off the top: leave it where it went
  const f=live?y:0;
  hero.querySelectorAll(":scope>img,:scope>video").forEach(m=>{m.style.translate=f?`0 ${(f*.35).toFixed(1)}px`:""});
  heroCopy.style.translate=f?`0 ${(f*.2).toFixed(1)}px`:"";
  heroCopy.style.opacity=f?clamp(1-f/(h*.6),0,1).toFixed(3):"";
}
let queued=false;
function frame(){
  queued=false;
  const y=scrollY, vh=innerHeight, max=document.documentElement.scrollHeight-vh;
  nav.classList.toggle("solid",y>40);
  readBar.style.transform=`scaleX(${max>0?clamp(y/max,0,1).toFixed(4):0})`;
  let here=null; for(const [a,s] of spy) if(s.getBoundingClientRect().top<vh*.4) here=a;
  spy.forEach(([a])=>a===here?a.setAttribute("aria-current","location"):a.removeAttribute("aria-current"));
  heroFrame(y);
  for(const {img,box} of drifting){
    const r=box.getBoundingClientRect();
    if(r.bottom<-80||r.top>vh+80) continue;
    const t=(r.top+r.height/2-vh/2)/(vh/2+r.height/2);
    img.style.translate=`0 ${(-t*r.height*.06).toFixed(1)}px`;
  }
  garageFrame();
}
const queue=()=>{if(!queued){queued=true; requestAnimationFrame(frame);}};
addEventListener("scroll",queue,{passive:true});
addEventListener("resize",()=>{layoutGarage(); queue();});
if(document.fonts) document.fonts.ready.then(()=>{layoutGarage(); queue();});
layoutGarage(); frame();

/* hero telemetry: a scripted lap segment, looped */
(function(){
  const seg=[ // [seconds, speed km/h, throttle, brake, gear, lat g]
    [0,288,1,0,7,.2],[2.2,312,1,0,7,.1],[2.9,305,0,1,6,.4],[3.5,190,0,.85,4,1.2],[4.1,142,.2,.3,3,2.1],[4.9,138,.55,0,3,2.4],[5.8,176,.95,0,4,1.6],[7.0,232,1,0,5,.6],[8.4,270,1,0,6,.3],[9.6,288,1,0,7,.2]
  ];
  const T=seg[seg.length-1][0]; const $=id=>document.getElementById(id);
  let start=performance.now();
  function lerp(a,b,f){return a+(b-a)*f}
  function tick(now){
    const t=((now-start)/1000)%T; let i=0; while(i<seg.length-2&&seg[i+1][0]<t)i++;
    const a=seg[i],b=seg[i+1],f=(t-a[0])/(b[0]-a[0]);
    const spd=lerp(a[1],b[1],f),thr=lerp(a[2],b[2],f),brk=lerp(a[3],b[3],f),lat=lerp(a[5],b[5],f),gear=f<.5?a[4]:b[4];
    const ffb=Math.min(1,.25+lat*.28+Math.sin(now/37)*.03);
    $("t-thr").style.width=thr*100+"%"; $("t-thr-v").textContent=Math.round(thr*100)+"%";
    $("t-brk").style.width=brk*100+"%"; $("t-brk-v").textContent=Math.round(brk*100)+"%";
    $("t-ffb").style.width=ffb*100+"%"; $("t-ffb-v").textContent=ffb.toFixed(2);
    $("t-lat").style.width=Math.min(100,lat/3*100)+"%"; $("t-lat-v").textContent=lat.toFixed(1)+"g";
    $("t-spd").firstChild.nodeValue=Math.round(spd); $("t-gear").textContent=gear;
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
})();

// The hero plays a silent loop from the promo over the still, on screens wide
// enough to show it and only when the viewer has not asked for less motion or data.
(() => {
  const hero = document.querySelector('.hero');
  if (!hero || !SITE.heroLoop.length || !matchMedia('(min-width:641px)').matches
      || matchMedia('(prefers-reduced-motion:reduce)').matches
      || (navigator.connection && navigator.connection.saveData)) return;
  const v = document.createElement('video');
  Object.assign(v, {muted: true, loop: true, playsInline: true, autoplay: true, preload: 'auto'});
  v.setAttribute('aria-hidden', 'true');
  for (const [src, type] of SITE.heroLoop) {
    const s = document.createElement('source'); s.src = src; s.type = type; v.append(s);
  }
  v.addEventListener('playing', () => v.classList.add('on'), {once: true});
  hero.querySelector(':scope>img').after(v);
})();
// The trailer opens over the page; YouTube's player is only loaded on the click.
// Without script (or <dialog>) the button is a plain link to the video.
document.querySelectorAll('[data-trailer]').forEach(a => a.addEventListener('click', e => {
  if (!window.HTMLDialogElement) return;
  e.preventDefault();
  const d = document.createElement('dialog');
  d.className = 'trailer';
  d.innerHTML = `<button type="button">Close ✕</button><iframe title="ApexSim trailer" allow="autoplay; encrypted-media; picture-in-picture; fullscreen" allowfullscreen
    src="https://www.youtube-nocookie.com/embed/${a.dataset.trailer}?autoplay=1&rel=0&modestbranding=1&playsinline=1"></iframe>`;
  const hero = document.querySelector('.hero>video');
  d.querySelector('button').onclick = () => d.close();
  d.addEventListener('click', ev => { if (ev.target === d) d.close(); });
  d.addEventListener('close', () => { d.remove(); hero && hero.play(); });
  hero && hero.pause();
  document.body.append(d);
  d.showModal();
}));

// A picture with a large version (`data-zoom`, from site/media.yml's `zoom`)
// opens over the page on a click or Enter, with the others of its gallery an
// arrow away. The large file is only fetched then: the picture already on the
// page stands in until it has loaded.
(() => {
  if (!window.HTMLDialogElement) return;
  const GALLERIES = '.shots,.screens,.band,.details,.liv-grid,.cars';
  let box = null, group = [], at = 0;
  const captionOf = img => img.closest('figure')?.querySelector('figcaption')?.textContent || img.alt || '';
  function show(i) {
    at = (i + group.length) % group.length;
    const small = group[at], big = box.querySelector('img');
    big.src = small.currentSrc || small.src;
    big.alt = small.alt;
    box.querySelector('figcaption').textContent = captionOf(small);
    const full = new Image();
    full.onload = () => { if (box && group[at] === small) big.src = full.src; };
    full.src = small.dataset.zoom;
  }
  function open(img) {
    group = [...(img.closest(GALLERIES) || document).querySelectorAll('img[data-zoom]')];
    box = document.createElement('dialog');
    box.className = 'lightbox' + (group.length < 2 ? ' single' : '');
    box.innerHTML = `<button type="button" class="x">Close ✕</button>
      <button type="button" class="prev" aria-label="Previous picture">‹</button>
      <figure><img alt=""><figcaption></figcaption></figure>
      <button type="button" class="next" aria-label="Next picture">›</button>`;
    box.addEventListener('click', e => {
      if (e.target.closest('.prev')) show(at - 1);
      else if (e.target.closest('.next')) show(at + 1);
      else box.close();
    });
    box.addEventListener('keydown', e => {
      if (e.key === 'ArrowLeft') show(at - 1);
      if (e.key === 'ArrowRight') show(at + 1);
    });
    box.addEventListener('close', () => { box.remove(); box = null; img.focus({preventScroll: true}); });
    document.body.append(box);
    show(group.indexOf(img));
    box.showModal();
  }
  document.addEventListener('click', e => {
    const img = e.target.closest('img[data-zoom]');
    if (img && !box) open(img);
  });
  document.addEventListener('keydown', e => {
    if (e.key === 'Enter' && !box && document.activeElement?.matches('img[data-zoom]')) open(document.activeElement);
  });
})();
