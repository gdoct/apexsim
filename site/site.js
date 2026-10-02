// The page's script. Everything it lists comes from `SITE`, which
// scripts/site/build_site.py writes into assets/data.js from the game's own
// content: the cars, the circuits, the class blurbs.
const DATA = SITE, CLASSES = SITE.classes, ORDER = SITE.order;
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
const tabs=document.getElementById("classTabs"), grid=document.getElementById("carGrid");
function showClass(k){
  [...tabs.children].forEach(b=>b.setAttribute("aria-selected",b.dataset.k===k));
  document.getElementById("classTitle").textContent=CLASSES[k].title;
  document.getElementById("classText").textContent=CLASSES[k].text;
  const cars=DATA.cars.filter(c=>c.cls===k).sort((a,b)=>!!b.img-!!a.img||b.hp-a.hp);
  grid.innerHTML=cars.map((c,i)=>`
    <article class="car" style="animation-delay:${i*60}ms">
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
    </article>`).join("");
}
ORDER.forEach(k=>{
  const n=DATA.cars.filter(c=>c.cls===k).length;
  const b=document.createElement("button");
  b.setAttribute("role","tab"); b.dataset.k=k; b.innerHTML=`${CLASSES[k].title}<small>${n}</small>`;
  b.onclick=()=>showClass(k); tabs.appendChild(b);
});
showClass(ORDER[0]);

/* tracks */
const tg=document.getElementById("trackGrid"), tf=document.getElementById("trackFilters");
const tracks=[...DATA.tracks].sort((a,b)=>a.name.localeCompare(b.name));
tg.innerHTML=tracks.map(t=>`
  <button class="trk" data-stem="${t.stem}" data-cat="${esc(t.cat)}">
    <svg viewBox="-6 -6 212 212" aria-hidden="true"><path d="${t.path}"/></svg>
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
let anim=null;
function selectTrack(stem){
  const t=DATA.tracks.find(x=>x.stem===stem); if(!t) return;
  tg.querySelectorAll(".trk").forEach(x=>x.setAttribute("aria-current",x.dataset.stem===stem));
  ["bg","path","run"].forEach(k=>ft[k].setAttribute("d",t.path));
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
  const reduce=matchMedia("(prefers-reduced-motion: reduce)").matches;
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

/* nav + reveal */
const nav=document.getElementById("nav");
addEventListener("scroll",()=>nav.classList.toggle("solid",scrollY>40),{passive:true});
const io=new IntersectionObserver(es=>es.forEach(e=>{if(e.isIntersecting){e.target.classList.add("in");io.unobserve(e.target)}}),{threshold:.12});
document.querySelectorAll(".reveal").forEach(el=>io.observe(el));
setTimeout(()=>document.querySelectorAll(".reveal:not(.in)").forEach(el=>{const r=el.getBoundingClientRect();if(r.top<innerHeight)el.classList.add("in")}),1500);

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
