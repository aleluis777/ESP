const ip_ss=""//http://192.168.0.165"
let data = new Object();
let delay=4000;
let btn=0;
var cnt=10;
var datos=[[],[],[],[]]
let click=false;

function monitoreo(data){
    console.log("datos")
    console.log(datos)
    console.log(data.d1)
    datos[0][datos[0].length]=data.d1;  if(datos[0].length>10) datos[0]=datos[0].slice(1,11)
    datos[1][datos[1].length]=data.d2;  if(datos[1].length>10) datos[1]=datos[1].slice(1,11)
    datos[2][datos[2].length]=data.d3;  if(datos[2].length>10) datos[2]=datos[2].slice(1,11)
    datos[3][datos[3].length]=data.d4;  if(datos[3].length>10) datos[3]=datos[3].slice(1,11)
    console.log(datos)
    myChart.update();
}
setInterval(function(){
    switch (btn){
        case 0:
            if(cnt==10){
                cnt=0
                //monitoreo({d1:23.4,d2:34.6})
                fetch(ip_ss+"/monitoreo", {
                    method: 'GET',
                    
                    headers: {'Content-Type': 'application/json'}
                    })
                    .then(response => response.json())
                    .then(response => monitoreo(response));
                }
            cnt=cnt+1
            break;
        default:
            //console.log("nunguno");
    } 

}, 300);



  var ctx = document.getElementById('myChart').getContext('2d');
   var myChart = new Chart(ctx, {
	   type: 'line',
	   data: {
		 labels: [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
         options: {       
            responsive: true,
            plugins: {
              title: {
                display: true,
                text: 'Min and Max Settings'
              }
            },
            scales: {
              y: {
                min: 10,
                max: 50,
              }
            }
          },
		 datasets: [{ 
			 data: datos[0],
			 label: "S1",
			 borderColor: "rgb(62,149,205)",
			 backgroundColor: "rgb(62,149,205,0.1)",
		   }, { 
			 data: datos[1],
			 label: "S2",
			 borderColor: "rgb(60,186,159)",
			 backgroundColor: "rgb(60,186,159,0.1)",
		   }, { 
			 data: datos[2],
			 label: "S3",
			 borderColor: "rgb(255,165,0)",
			 backgroundColor:"rgb(255,165,0,0.1)",
		   }, { 
			 data: datos[3],
			 label: "S4",
			 borderColor: "rgb(196,88,80)",
			 backgroundColor:"rgb(196,88,80,0.1)",
		   }
		 ]
	   },
	 });


