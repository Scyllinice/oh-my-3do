#include "game_info.h"
#include "usb_art.h"
#include <string.h>
static unsigned char lower(unsigned char c){return c>='A'&&c<='Z'?(unsigned char)(c+32):c;}
void game_info_filename(const char *path,char *name){
 static const char digits[]="0123456789abcdef";unsigned long crc=0xffffffffUL;size_t i;
 for(i=0;path[i];++i){unsigned char c=lower((unsigned char)path[i]);crc=usb_art_crc32_update(crc,&c,1);}
 crc^=0xffffffffUL;strcpy(name,"info/");for(i=0;i<8;++i)name[5+i]=digits[(crc>>(28-i*4))&15];strcpy(name+13,".txt");
}
int game_info_parse(const char *bytes,size_t length,const char *path,char *description,size_t capacity){
 const char *body,*end,*text,*tail;size_t i,n,path_size=strlen(path);unsigned long expected=0;
 if(!capacity)return -1;
 description[0]=0;
 if(length<21 || memcmp(bytes,"OM3DINFO1\n",10))return -1;
 body=bytes+10;end=(const char *)memchr(body,'\n',length-10);if(!end||(size_t)(end-body)!=path_size)return -1;
 for(i=0;i<path_size;++i)if(lower((unsigned char)body[i])!=lower((unsigned char)path[i]))return -1;
 text=end+1;end=(const char *)memchr(text,'\n',(size_t)(bytes+length-text));if(!end)return -1;
 n=(size_t)(end-text);if(n>=capacity||n>=GAME_DESCRIPTION_BYTES)return -1;
 tail=end+1;if(bytes+length-tail!=9||tail[8]!='\n')return -1;
 for(i=0;i<8;++i){unsigned char c=lower((unsigned char)tail[i]);if(c>='0'&&c<='9')c=(unsigned char)(c-'0');else if(c>='a'&&c<='f')c=(unsigned char)(c-'a'+10);else return -1;expected=(expected<<4)|c;}
 if(usb_art_crc32((const unsigned char *)body,(size_t)(tail-body))!=expected)return -1;
 for(i=0;i<n;++i)if((unsigned char)text[i]<32||(unsigned char)text[i]>126)return -1;
 memcpy(description,text,n);description[n]=0;return 0;
}
