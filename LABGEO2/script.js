const ip_ss=""//http://192.168.0.165"
let data = new Object();
let delay=4000;
let btn=0;
var cnt=10;
let click=false;
function datos(r){
    console.log(r)
}

function changeAA(ruta,n,m) {
    fetch(ip_ss+ruta+"?dato="+n+"&valor="+m, {
        method: 'GET',
        headers: {'Content-Type': 'text/plain'}
        }).then(response => alert("dato enviado")
    )
}

function changeIP(ruta,n,ip1,ip2,ip3,ip4) {
    fetch(ip_ss+ruta+"?dato="+n+"&ip1="+ip1+"&ip2="+ip2+"&ip3="+ip3+"&ip4="+ip4, {
        method: 'GET',
        headers: {'Content-Type': 'text/plain'}
        }).then(response => alert("dato enviado")
    )
}

function SDt2(p,n) {
    let m= n==null?0:document.getElementById(n).value
    console.log(p,"  ",m)
    if(p=="5" || p=="6" ||p=="7" ||p=="8") {
        console.log(m)
        const ip = m.split(".");
        changeIP("/sag3",p,ip[0],ip[1],ip[2],ip[3])
    }
    else if(p=="1" || p=="2" ||p=="3" ||p=="4") {
        changeAA("/sag2",p,parseInt(m*100));
    }
    else changeAA("/sag2",p,m);
}

function SDt3(p,n) {
    let m=document.getElementById(n).value
    console.log(p,"  ",m)
    changeAA("/temp",p,m)
}

function SDt4(p,slv,rg,dt) {
    const s=document.getElementById(slv).value
    const r=document.getElementById(rg).value
    const g=document.getElementById(dt).value
    const m=s+","+r+","+g
    fetch(ip_ss+"/modbus"+"?slave="+s+"&reg="+r+"&dat="+g, {
        method: 'GET',
        headers: {'Content-Type': 'text/plain'}
        }).then(response => alert("dato enviado")
    )
}

function SDt(p,n) {
    let m=document.getElementById(n).textContent
    m=(m=="ON"?0:1)
    if(p=="7"){
        m==1?(m=0):(m=1)
    }
    changeAA("/sag",p,m)
}
function Label(b) {
    cnt=10
    btn=b
    click=true;
}

function monitoreo(r){
    Wh(r.d1,"w1d1");
    Wh(r.d2,"w1d2");
    Wh(r.d3,"w1d3");
    Wh2(r.d4,"w1d4");
    Wh2(r.d5,"w1d5");
    (r.d6 || r.d6==0)?document.getElementById("w1d6").innerHTML = getTimeOffline(parseInt(r.d6)):null;
    (r.d13|| r.d13==0)?document.getElementById("w1d13").innerHTML = getTimeOffline(parseInt(r.d13)):null;

    
    
}

function DateFecha(d,id){
    const date = new Date(d*1000);
    console.log(date)
    console.log(date.toString())
    document.getElementById(id).innerHTML = date.toString();
}
function Wh(d,id){d?document.getElementById(id).innerHTML = d:document.getElementById(id).innerHTML = 0;}
function En(d,id){d?document.getElementById(id).innerHTML = (parseInt(d)/3600).toFixed(2):document.getElementById(id).innerHTML = 0;}
//Leed AA on-off
function Wh2(d,id){
    if(d=="1" || d=="0"){
        document.getElementById(id).innerHTML = d?"ON":"OFF";
        document.getElementById(id+"C").style.background  = d=="1"?"green":"red";
    }
}

function Alrm(d,id){d!=0?document.getElementById(id).innerHTML = "ALARMADO":document.getElementById(id).innerHTML = "NORMAL";}


//Parametors set y actualizacines
function Wh3(d,id){d?document.getElementById(id).value = d:null;}

function params(r){
    Wh3(r.i1,"w2d1");
    Wh3(r.i2,"w2d2");
    Wh3(r.i3,"w2d3");
    Wh3(r.i4,"w2d4");
    Wh3(r.i9,"w2d5");

    Wh3(r.i5,"w3d1");
    Wh3(r.i6,"w3d2");
    Wh3(r.i7,"w3d3");
    Wh3(r.i8,"w3d4");
    
    
}

function modbus(r){
    Wh(r.d1,"w4d1");
    Wh(r.d2,"w4d2");
    Wh(r.d3,"w4d3");
    Wh(r.d4,"w4d4");
    Wh(r.d5,"w4d5");
    Wh(r.d6,"w4d6");
    Wh(r.d7,"w4d7");
    En(r.d8,"w4d8");
    Alrm(r.d9,"w4d9");
    Alrm(r.d10,"w4d10");
    Alrm(r.d11,"w4d11");
    Alrm(r.d12,"w4d12");

    Alrm(r.d13,"w4d13");
    Alrm(r.d14,"w4d14");
    
}


setInterval(function(){
    switch (btn){
        case 0:
            if(cnt>4){
                cnt=0
                fetch(ip_ss+"/monitoreo", {
                    method: 'GET',
                    //mode: "no-cors", // no-cors, *cors, same-origin
                    headers: {'Content-Type': 'application/json'}
                    })
                    .then(response => response.json())
                    .then(response => monitoreo(response));
                }
            cnt=cnt+1
            break;
        case 1:
            if(click){
                fetch(ip_ss+"/params", {
                    method: 'GET',
                    //mode: "no-cors", // no-cors, *cors, same-origin
                    headers: {'Content-Type': 'application/json'}
                    })
                    .then(response => response.json())
                    .then(response => params(response));
                click=false
            }
            break;
        case 2:
            //console.log("el boton es 2");
            break;
        case 3:
            if(click){
                fetch(ip_ss+"/params", {
                    method: 'GET',
                    //mode: "no-cors", // no-cors, *cors, same-origin
                    headers: {'Content-Type': 'application/json'}
                    })
                    .then(response => response.json())
                    .then(response => params(response));
                click=false
            }
            break;
        case 4:
            if(cnt>4){
                cnt=0
                fetch(ip_ss+"/energia", {
                    method: 'GET',
                    //mode: "no-cors", // no-cors, *cors, same-origin
                    headers: {'Content-Type': 'application/json'}
                    })
                    .then(response => response.json())
                    .then(response => modbus(response));
            }
            cnt=cnt+1
            break;
        default:
            //console.log("nunguno");
    }

}, 300);

function Fecha() {
    const s=document.getElementById("fecha").value

    var dt = new Date(s)

    console.log(dt.getTime())
    const n=20;
    fetch(ip_ss+"/sag2?dato="+n+"&valor="+(dt.getTime()/1000), {
        method: 'GET',
        headers: {'Content-Type': 'text/plain'}
        }).then(response => alert("dato enviado")
    )
    
}

function getTimeOffline(s) {
    var h = Math.floor(s / 3600);
    if(h!=0) h = (h < 10)? '' + h+ 'h': h+ 'h';
    var m = Math.floor((s / 60) % 60);
    if(m!=0) m = (m < 10)? '' + m + 'm': m + 'm';
    var S = Math.floor((s) % 60);
    S = (S < 10)? '' + S +'s': S+'s';
    return (h?h:'')+' '+(m?m:'')+' '+(S?S:'');
  }


