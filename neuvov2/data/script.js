
const sidebar = document.getElementById('sidebar');
const hamburguesa = document.getElementById('hamburguesa');

hamburguesa.addEventListener('click', () => {
  sidebar.classList.toggle('active');
});

// Funcionalidad de pestañas
function showTab(tabId) {
  document.querySelectorAll('.tabcontent').forEach(tab => {
    tab.classList.remove('active');
  });
  
  document.querySelectorAll('.nav-button').forEach(btn => {
    btn.classList.remove('active');
  });

  document.getElementById(tabId).classList.add('active');
  event.currentTarget.classList.add('active');
  
  if (window.innerWidth <= 768) {
    sidebar.classList.remove('active');
  }
}


const ip_ss="" 
let data = new Object();
let delay=4000;
let btn=0;
let offline=false;
var cnt=5;
let click=false;
let intermitente=true


var config_data={}

function monitoreo(r){
  WhSAG(r.i1,"i1");
  WhSAG(r.i2,"i2");
  WhSAG(r.i3,"i3");
  WhSAG(r.i4,"i4");
  WhSAG(r.i5,"i5");
  WhSAG(r.i6,"i6");
  
  Wh2(r.i7,"i7");
  Wh2(r.i8,"i8");

  Wh3(r.i9,"i9");

  WhSAG(r.i10,"i10");
  WhSAG(r.i11,"i11");
  WhSAG(r.i12,"i12");
  WhSAG(r.i13,"i13");
  WhSAG(r.i14,"i14");
  WhSAG(r.i15,"i15");
  
  Wh2SAG(r.o1,"o1");
  Wh2SAG(r.o2,"o2");
  Wh2SAG(r.o5,"o5");
  Wh2SAG(r.o4,"o4");
  r.t?document.getElementById("t").innerHTML = getTimeOffline(parseInt(r.t)):null;  
  DateFecha(r.d,"date");
  Version(r.v,"version");
}

function Wh(d,id){d?document.getElementById(id).innerHTML = d:document.getElementById(id).innerHTML = 0;}

function WhSAG(d,id){
  const clave = 1;
  var doc=document.getElementById(id);
  var gauge=document.getElementById("gauge"+id);
  d?doc.innerHTML=(d==-1?"error  --":d) :doc.innerHTML = 0;
  (d && d!=-1)?gauge.style.setProperty('--rotation', (2*d)+'deg'):null;
}

function En(d,id){d?document.getElementById(id).innerHTML = (parseInt(d)/3600).toFixed(2):document.getElementById(id).innerHTML = 0;}
//Leed AA on-off
function Wh2(d,id){
  if(d=="1" || d=="0"){
      document.getElementById(id).innerHTML = d?"ON":"OFF";
      document.getElementById("c"+id).style.background  = d=="1"?"red":"#a4a4a4";
  }
}

function Wh3(d,id){
  
  document.getElementById(id).innerHTML = d?d:"--";
      
}

function Wh2SAG(d,id){
  if(d=="1" || d=="0"){
      document.getElementById(id).innerHTML = d?"ON":"OFF";
      document.getElementById("hw"+id).checked = (d=="1"?true:false);
      //document.getElementById(id+"C").style.background  = d=="1"?"green":"red";
  }
}

function Version(d,id){
  d?document.getElementById(id).innerHTML = "version firmware: "+d.toFixed(1):"--";
}
 

function DateFecha(d, id) {
  const date = new Date(d * 1000); // Convertir el timestamp a milisegundos

  // Opciones para formatear la fecha en español
  const options = {
    weekday: 'long', // Día de la semana (ej: "viernes")
    year: 'numeric', // Año (ej: "2023")
    month: 'long',   // Mes (ej: "enero")
    day: 'numeric',  // Día del mes (ej: "20")
    hour: 'numeric', // Hora (ej: "22")
    minute: 'numeric', // Minutos (ej: "04")
    second: 'numeric', // Segundos (ej: "42")
    hour12: true, // Usar formato de 12 horas (opcional)
  };

  // Formatear la fecha en español
  const formatter = new Intl.DateTimeFormat('es-PE', options);
  const fechaFormateada = formatter.format(date);

  // Mostrar la fecha en el elemento HTML
  document.getElementById(id).innerHTML = fechaFormateada;
}

function PressButton(id){
  const data=!document.getElementById("hwo"+id).checked?1:0;
  fetch(ip_ss+"/api/data", {
    method: 'POST',
    body: JSON.stringify({button:id,data:data}),
    headers: {'Content-Type': 'application/json'}
    })
    .then(response => mostrarToast("Control "+(data?"activado":"desactivado")))
    .catch(error => {
      errorToast("Error en la solicitud");
    })
    ;
      //document.getElementById(id+"C").style.background  = d=="1"?"green":"red";
  
}
function Calibracion() {
  var botton = document.getElementById("nSelect-calibracion").value;
  var data = document.getElementById("calibracion-input").value;

  let floatValue = parseFloat(data);
  fetch(ip_ss+"/api/set", {
    method: 'POST',
    body: JSON.stringify({button:botton,data:parseInt(floatValue*100)}),
    headers: {'Content-Type': 'application/json'}
    })
    .then(response => mostrarToast("Seteo de dato  correctamente"))
    .catch(error => {
      errorToast("Error en la solicitud");
    })
    ;

  // Aquí puedes agregar el código para enviar los valores a tu ESP32 o realizar otras acciones
}

function Fecha(id){
  const s=document.getElementById("fecha").value
    var dt = new Date(s)
    console.log(dt.getTime())

  fetch(ip_ss+"/api/set", {
    method: 'POST',
    body: JSON.stringify({button:id,data:dt/1000}),
    headers: {'Content-Type': 'application/json'}
    })
    .then(response => mostrarToast("Fecha cambiada correctamente"))
    .catch(error => {
      errorToast("Error en la solicitud");
    })
    ;
      //document.getElementById(id+"C").style.background  = d=="1"?"green":"red";
  
}

function Restart(button){
  fetch(ip_ss+"/api/restart", {
    method: 'POST',
    body: JSON.stringify({button,data:"0"}),
    headers: {'Content-Type': 'application/json'}
    })
    .then(response => mostrarToast("Reiniciando..",3000))
    .catch(error => {
      errorToast("Error en la solicitud");
    })
    ;
      //document.getElementById(id+"C").style.background  = d=="1"?"green":"red";
  
}

function mostrarToast(mensaje, duracion = 1000) {
    const toast = document.getElementById('toast');
    toast.textContent = mensaje;
    toast.className = 'toast-visible';

    // Ocultar después de la duración
    setTimeout(() => {
        toast.className = 'toast-hidden';
    }, duracion);
}

function errorToast(mensaje, duracion = 1000) {
  const toast = document.getElementById('toast');
  toast.textContent = mensaje;
  toast.className = 'toast-visible-error';

  // Ocultar después de la duración
  setTimeout(() => {
      toast.className = 'toast-hidden';
  }, duracion);
}

setInterval(function(){
  switch (btn){
      case 0:
          if(cnt==5){
              cnt=0
              // monitoreo(dat)
              fetch(ip_ss+"/monitoreo", {
                  method: 'GET',
                  //mode: "no-cors", // no-cors, *cors, same-origin
                  headers: {'Content-Type': 'application/json'}
                  })
                  .then(response => response.json())
                  .then(response => {offline=false;monitoreo(response)})
                  .catch(error => {
                    offline=true;
                  })
                  ;
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
          console.log("el boton es 2");
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
          if(cnt==10){
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

}, 500);

setInterval(function(){
  let color=offline?"#ff9800":"#0cb517"
  const s = new Date();
  if(s.getSeconds()%2==0){color=offline?"#cb3f13":"#027209"}
  document.getElementById("online").style.background = color;
}, 1000);

function openCity(evt, cityName) {
  var i, tabcontent, tablinks;
  tabcontent = document.getElementsByClassName("tabcontent");
  for (i = 0; i < tabcontent.length; i++) {
    tabcontent[i].style.display = "none";
  }
  tablinks = document.getElementsByClassName("tablinks");
  for (i = 0; i < tablinks.length; i++) {
    tablinks[i].className = tablinks[i].className.replace(" active", "");
  }
  document.getElementById(cityName).style.display = "block";
  evt.currentTarget.className += " active";
}

function toggleMenu() {
  const menu = document.querySelector('.menu');
  menu.classList.toggle('active');
}

var objeto_monitoreo=[
  {nm:"Piloto ColdSmart Gilat 2024",stp:0,tp:0,en:true,u:"°C"},

  {nm:"AA 1",stp:4,tp:1,en:true,u:"°C"},
  {nm:"AA 2",stp:4,tp:2,en:true,u:"°C"},
  {nm:"BPS ",stp:4,tp:5,en:true,u:"°C"},
  {nm:"AT",stp:4,tp:4,en:true,u:"°C"},

  {nm:"T. Operación 1",c:"#1d7a91",stp:1,tp:1,en:true,u:"°C"},
  {nm:"T. Operación 2",c:"#ff5722",stp:1,tp:2,en:true,u:"°C"},
  {nm:"T. Inyección 1",c:"#ad3724",stp:1,tp:3,en:true,u:"°C"},
  {nm:"T. Inyección 2",c:"#9c27b6",stp:1,tp:4,en:true,u:"°C"},
  {nm:"H. Gestor",c:"#0b3682",stp:1,tp:5,en:true,u:"%"},
  {nm:"T. Gestor",c:"#38ba3c",stp:1,tp:6,en:true,u:"°C"},

  {nm:"High Temp.",stp:2,tp:7,en:true,u:"°C"},
  {nm:"Bypass",stp:2,tp:8,en:false,u:"°C"},
  {nm:"Secuencia",stp:3,tp:9,en:true,u:""},

  {nm:"IR",c:"#d4002b",stp:1,tp:10,en:true,u:"A"},
  {nm:"IS",c:"#0c0980",stp:1,tp:11,en:true,u:"A"},
  {nm:"IT",c:"#343a40",stp:1,tp:12,en:true,u:"A"},
  {nm:"VRS",c:"#d4002b",stp:1,tp:13,en:true,u:"V"},
  {nm:"VST",c:"#0c0980",stp:1,tp:14,en:true,u:"V"},
  {nm:"VRT",c:"#343a40",stp:1,tp:15,en:true,u:"V"},
]

  //{s1,s1,s1,s1,}    °C es unicad de centigrados

function Sensor(n){
  if (n.stp==1){
    return (`
      <div class="card">
          <span>`+n.nm+`</span> 
          
          <div style="display: flex;">
          <p><b id="i`+n.tp+`"> -- </b></p><p>`+n.u+`</p>
          </div> 
          <div id="gaugei`+n.tp+`" class="gauge" style="width: 120px; --rotation:0deg; --color:`+n.c+`; --background:#e9ecef;">
            <div class="percentage"></div>
            <div class="mask"></div>
            <span class="value">.</span>
          </div> 
      </div>`)
  }
  else if (n.stp==2){
    return (`
      <div class="card">
          <span>`+n.nm+`</span> 
          <p id="i`+n.tp+`"> -- </p>
          <div class="content-iot">
            <div class="status_in" id="ci`+n.tp+`"></div>
          </div>
      </div>`)
  }
  else if (n.stp==3){
    return (`
      <div class="card">
        <span>`+n.nm+`</span>  
        <div class="content-iot">
          <p class="number_in" id="i`+n.tp+`"> - </p>
        </div>
      </div>  `)
  }
  else if (n.stp==4)
    return (`
    <div class="card">
      <span>`+n.nm+`</span> 
      <p id="o`+n.tp+`"> -- </p>
      <div class="content-iot">
        <label class="switch"   onclick="PressButton('`+n.tp+`')" >
          <input type="checkbox" id="hwo`+n.tp+`" disabled>
          <span class="slider round"></span>
           
        </label>
      </div>
    </div>`)
  else return (``)
}

function subtyoe(n,subtype,k){
  if(n>=1  && n<=4)return(`<select  class="text-input" value ="`+subtype+`" id="config_subtype`+k+`" >
      <option `+(subtype==1?"selected": "")+` value="1">PT100</option>
      <option `+(subtype==2?"selected": "")+` value="2">ANIEGO</option>
      <option `+(subtype==3?"selected": "")+` value="3">INPUT_1</option>
      <option `+(subtype==4?"selected": "")+` value="4">INPUT_0</option>
  </select>`)
  else if(n>=5  && n<=9)return  (`<select class="text-input" value ="`+subtype+`"  id="config_subtype`+k+`">
          <option `+(subtype==1?"selected": "")+` value="1">T_D</option>
          <option `+(subtype==2?"selected": "")+` value="2">H_H</option>
          <option `+(subtype==3?"selected": "")+` value="3">INPUT_1</option>
          <option `+(subtype==4?"selected": "")+` value="4">INPUT_0</option>
      </select>`)
      else if(n>=51  && n<=54)return  (`<select class="text-input" value ="`+subtype+`"  id="config_subtype`+k+`">
      <option `+(subtype==1?"selected": "")+` value="1">ALARMA/ON</option>
      <option `+(subtype==2?"selected": "")+` value="2">ALARMA/Off</option>
      <option `+(subtype==3?"selected": "")+` value="3">ARRANQUE/ON</option>
      <option `+(subtype==4?"selected": "")+` value="4">ARRANQUE/Off</option>
  </select>`)
  else return  (`<select  class="text-input" value ="1" id="config_subtype`+k+`" hidden>
  <option `+(subtype==1?"selected": "")+` value>PT100</option>
</select>`)
}

function Tabla(n,k){
  return (`
  <tr>
      <th scope="row"><div class="text-wrapper"><input class="text-input"  id="config_name`+k+`" value="`+n.nm+`"></th>
      <td><div class="text-wrapper"><select class="text-input" name="Control" id="config_hw`+k+`"  `+(n.tp==0?"hidden":"")+` value="`+n.tp+`" onchange="change_subtype('`+k+`')" >
          <option `+(n.tp==1?"selected": "")+` value="1">In 1</option>
          <option `+(n.tp==2?"selected": "")+` value="2">In 2</option>
          <option `+(n.tp==3?"selected": "")+` value="3">In 3</option>
          <option `+(n.tp==4?"selected": "")+` value="4">In 4</option>
          <option `+(n.tp==5?"selected": "")+` value="5">In 5</option>
          <option `+(n.tp==6?"selected": "")+` value="6">In 6</option>
          <option `+(n.tp==7?"selected": "")+` value="7">In 7</option>
          <option `+(n.tp==8?"selected": "")+` value="8">In 8</option>
          <option `+(n.tp==9?"selected": "")+` value="9">In 9</option>
          <option `+(n.tp==51?"selected": "")+` value="51">Out 1</option>
          <option `+(n.tp==52?"selected": "")+` value="52">Out 2</option>
          <option `+(n.tp==53?"selected": "")+` value="53">Out 3</option>
          <option `+(n.tp==54?"selected": "")+` value="54">Out 4</option>
      </select></div></td>
      <td><div class="text-wrapper">`+subtyoe(n.tp,n.stp,k)+`</div></td>
      <td><div class=" "><input class="text-input" type="checkbox" id="config_enabled`+k+`" `+(n.en?"checked":"")+` ></div></td>
      <td><button class="button-input" onclick='Delete_control2(`+k+`)' class="inputbuton"> x</button></td>
  </tr>`)
}

function DashboardGet(data){
  console.log(data)
  objeto_monitoreo=data.slice()
  let html=``;
  let html2=``;
  data.map(function(item,k) {
      html=html+Sensor(item);
      html2=html2+Tabla(item,(k+1));

  })
  var capa = document.getElementById('dashboard_io');
  capa.setAttribute('class', 'cards');
  capa.innerHTML = html;

  // entradas y salides momento stop
  //var capa2 = document.getElementById('setuo_io');
  //capa2.setAttribute('class', '');
  //capa2.innerHTML = html2;

  console.log("DashboardGet")
  openCity(event, 'option1')
  
}

function TablaConfig(name,value,k){
  return (`
  <tr>
      <th scope="row">
        <div>
          <p class="">`+name+`</p>
        </div>
      </th>
      <th >
        <div class="text-wrapper">
          <input class="text-input"  id="`+k+`" value="`+value+`">
        </div>
      </th>
  </tr>`)
}

function DashboardConfig(data){
  let html2=``;
  
  html2=html2+TablaConfig("IP",data.ip.join("."),"ip");
  html2=html2+TablaConfig("GATEWAY",data.gateway.join("."),"gateway");
  html2=html2+TablaConfig("SUBNET",data.subnet.join("."),"subnet");
  html2=html2+TablaConfig("MAC",data.mac.join(":"),"mac");
  var capa2 = document.getElementById('config_io');
  capa2.setAttribute('class', '');
  capa2.innerHTML = generarTable(html2,"Configuracion de red")

  html2=""; 
  html2=html2+TablaConfig("Comunidad SNMP",data.snmpcommunity,"snmpcommunity"); 
  html2=html2+TablaConfig("Nombre del equipo",data.device,"device");  
  //html2=html2+TablaConfig("Puerto SNMP",data.snmpport,"snmpport"); 
  capa2.innerHTML = capa2.innerHTML+generarTable(html2,"Configuracion snmp")

  html2=""; 
  html2=html2+TablaConfig("Setpoint Minimo",data.setpoints[0],"setpoints1"); 
  html2=html2+TablaConfig("Setpoint Maximo",data.setpoints[1],"setpoints2"); 
  html2=html2+TablaConfig("Setpoint AT",data.setpoints[2],"setpoints3"); 
  html2=html2+TablaConfig("Setpoint BPS",data.setpoints[3],"setpoints4");  
  //html2=html2+TablaConfig("Seciencia actual",data.setpoints[3],"setpoints4");   
  capa2.innerHTML = capa2.innerHTML+generarTable(html2,"Configuracion gestor")
  

  data.device?document.getElementById("name").innerHTML = data.device:"Equipo Coldsmart";  
  
  
}
function getTimeOffline(s) {
  var h = Math.floor(s / 3600);
  if(h!=0) h = (h < 10)? '' + h+ 'h': h+ 'h';
  var m = Math.floor((s / 60) % 60);
  if(m!=0) m = (m < 10)? '' + m + 'm': m + 'm';
  var S = Math.floor((s) % 60);
  S = (S < 10)? '' + S +'s': S+'s';
  return 'Tiempo de actividad: '+ (h?h:'')+' '+(m?m:'')+' '+(S?S:'');
}
function generarTable(data,name){
  return `
      <table><thead><tr><th>`+name+`</th> </tr> </thead><tbody>`+data+`</tbody> </table>`;
}

ConfigGet();

DashboardGet(objeto_monitoreo);

function Add_form(id){
  let inputElement = document.getElementById(id).value;
  config_data[id]=inputElement;
  //console.log(id,inputElement)
}

function Add_form2(id){
  let inputElement = document.getElementById(id).value;
  let ipArrayNumber = inputElement.split(".").map(elemento => parseInt(elemento));
  config_data[id]=ipArrayNumber;
  //console.log(id,ipArrayNumber)
}

function Save_cambios2(){
  Add_form("snmpcommunity");
  Add_form("device");
  // Add_form("snmpport"); 
  Add_form2("ip");
  Add_form2("gateway");
  Add_form2("subnet");

  let set1 = parseFloat(document.getElementById("setpoints1").value);
  let set2 = parseFloat(document.getElementById("setpoints2").value);
  let set3 = parseFloat(document.getElementById("setpoints3").value);
  let set4 = parseFloat(document.getElementById("setpoints4").value);
  config_data["setpoints"]=[set1,set2,set3,set4];
  console.log(config_data);
  fetch(ip_ss+"/config", {
    method: 'POST',
    body: JSON.stringify(config_data),
    headers: {'Content-Type': 'application/json'}
    })
    .then(response => mostrarToast("datos guardados correctamente"))
    .catch(error => {
      errorToast("Error en la solicitud");
    })
    ;
  
}

function ConfigGet(){
  //alert("aqui")
  fetch(ip_ss+"/config.json", {
    method: 'GET',
    //mode: "no-cors", // no-cors, *cors, same-origin
    headers: {'Content-Type': 'application/json'}
    })
    .then(response => response.json())
    .then(response => {console.log(response);config_data=response;DashboardConfig(config_data);});
  
}