varying vec3 N;
varying vec3 v;

void main(void)  
{     
   v = vec3(gl_ModelViewMatrix * gl_Vertex);       
   N = normalize(gl_NormalMatrix * gl_Normal);
   
   gl_FrontColor = gl_Color;
   gl_BackColor = gl_Color;
   
   gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;  
} 