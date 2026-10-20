#include "menu_categories.h"
#include "menu_ui.h"
#include <string.h>

typedef struct Token { size_t start, end, next; int type; } Token;
typedef struct Row { char name[49]; size_t hidden; int hide; } Row;
typedef struct Categories { char text[MENU_CATEGORIES_BYTES]; Row rows[MENU_CATEGORIES_MAX]; size_t count; int configured; } Categories;
static int prefix(const char *path,const char *name) {
    while(*name) {unsigned char a=(unsigned char)*path++,b=(unsigned char)*name++;if(a>='A'&&a<='Z') a+=(unsigned char)('a'-'A');if(b>='A'&&b<='Z') b+=(unsigned char)('a'-'A');if(a!=b) return 1;}return 0;
}
static Categories current, pending;
static Token tokens[512];
static size_t token_count, cursor, input_length;
static const char *input;
static void spaces(void) { while (cursor<input_length && (input[cursor]==' ' || input[cursor]=='\n' || input[cursor]=='\r' || input[cursor]=='\t')) ++cursor; }
static int value(unsigned int depth)
{
    size_t slot, start;
    char c;
    if (depth>12 || token_count>=512) return -1;
    spaces(); if (cursor>=input_length) return -1;
    slot=token_count++; start=cursor; c=input[cursor++]; tokens[slot].type=c;
    if (c=='"') {
        while (cursor<input_length && input[cursor]!='"') {
            unsigned char ch=(unsigned char)input[cursor++];
            if (ch<32) return -1;
            if (ch=='\\') {
                if (cursor>=input_length) return -1;
                ch=(unsigned char)input[cursor++];
                if (!strchr("\"\\/bfnrtu",ch)) return -1;
                if (ch=='u') {unsigned int i;for(i=0;i<4;++i) {if(cursor>=input_length || !strchr("0123456789abcdefABCDEF",input[cursor++])) return -1;}}
            }
        }
        if (cursor>=input_length) return -1;
        ++cursor;
    } else if (c=='{' || c=='[') {
        char end=c=='{' ? '}' : ']'; spaces();
        if (cursor<input_length && input[cursor]==end) ++cursor;
        else for (;;) {
            if (c=='{') {spaces();if(cursor>=input_length || input[cursor]!='"' || value(depth+1)<0) return -1;spaces();if(cursor>=input_length || input[cursor++]!=':') return -1;}
            if (value(depth+1)<0) return -1;
            spaces();if(cursor>=input_length) return -1;
            if(input[cursor]==end) {++cursor;break;}
            if(input[cursor++]!=',') return -1;
        }
    } else {
        cursor=start;
        if (input_length-cursor>=4 && !memcmp(input+cursor,"true",4)) cursor+=4;
        else if (input_length-cursor>=5 && !memcmp(input+cursor,"false",5)) cursor+=5;
        else if (input_length-cursor>=4 && !memcmp(input+cursor,"null",4)) cursor+=4;
        else {if(input[cursor]=='-') ++cursor;if(cursor>=input_length || input[cursor]<'0' || input[cursor]>'9') return -1;while(cursor<input_length && input[cursor]>='0' && input[cursor]<='9') ++cursor;}
    }
    tokens[slot].start=start;tokens[slot].end=cursor;tokens[slot].next=token_count;return (int)slot;
}
static int equal(size_t index, const char *text) {return tokens[index].type=='"' && tokens[index].end-tokens[index].start==strlen(text)+2 && !memcmp(input+tokens[index].start+1,text,strlen(text));}
static size_t field(size_t object, const char *key)
{
    size_t i=object+1, found=512;
    if(tokens[object].type!='{') return 512;
    while(i<tokens[object].next) {size_t v=i+1;if(equal(i,key)) {if(found!=512) return 512;found=v;}i=tokens[v].next;}
    return found;
}
int menu_categories_parse(const char *text, size_t length)
{
    size_t array, schema, i;
    if(!text || !length || length>=MENU_CATEGORIES_BYTES) return -1;
    input=text;input_length=length;cursor=token_count=0;
    if(value(0)!=0 || tokens[0].type!='{') return -1;
    spaces();if(cursor!=length) return -1;
    schema=field(0,"schema");array=field(0,"categories");
    if(schema==512 || tokens[schema].end-tokens[schema].start!=1 || input[tokens[schema].start]!='1' || array==512 || tokens[array].type!='[') return -1;
    memset(&pending,0,sizeof(pending));
    for(i=array+1;i<tokens[array].next;i=tokens[i].next) {
        size_t name, hidden, id, n, j; Row *row;
        if(pending.count>=MENU_CATEGORIES_MAX || tokens[i].type!='{') return -1;
        name=field(i,"name");hidden=field(i,"hidden");id=field(i,"id");
        if(name==512 || hidden==512 || id==512 || tokens[name].type!='"' || tokens[id].type!='"') return -1;
        n=tokens[name].end-tokens[name].start-2;
        if(!n || n>48) return -1;
        row=&pending.rows[pending.count];memcpy(row->name,input+tokens[name].start+1,n);
        for(j=0;j<n;++j) if((unsigned char)row->name[j]<32 || (unsigned char)row->name[j]>=127 || strchr("<>:\"/\\|?*",row->name[j])) return -1;
        if(row->name[0]==' ' || row->name[n-1]==' ' || row->name[n-1]=='.' || !menu_path_compare(row->name,".ohmy3do")) return -1;
        for(j=0;j<pending.count;++j) if(!menu_path_compare(row->name,pending.rows[j].name)) return -1;
        row->hidden=tokens[hidden].start;
        if(tokens[hidden].end-row->hidden==4 && !memcmp(input+row->hidden,"true",4)) row->hide=1;
        else if(tokens[hidden].end-row->hidden!=5 || memcmp(input+row->hidden,"false",5)) return -1;
        ++pending.count;
    }
    if(!pending.count) return -1;
    memcpy(pending.text,text,length);pending.configured=1;current=pending;return 0;
}
void menu_categories_reset(void)
{
    static const char *names[]={"Games","Demos","Samplers","Applications","Multimedia","Educational","Bonus Discs","Adult"};
    size_t i;char number[2];number[1]=0;
    memset(&current,0,sizeof(current));strcpy(current.text,"{\"schema\":1,\"categories\":[");
    for(i=0;i<8;++i) {number[0]=(char)('0'+i);if(i) strcat(current.text,",");strcat(current.text,"{\"id\":\"category-");strcat(current.text,number);strcat(current.text,"\",\"name\":\"");strcat(current.text,names[i]);strcat(current.text,"\",\"hidden\":false}");}
    strcat(current.text,"],\"defaults\":{");
    for(i=0;i<8;++i) {number[0]=(char)('0'+i);if(i) strcat(current.text,",");strcat(current.text,"\"");strcat(current.text,names[i]);strcat(current.text,"\":\"category-");strcat(current.text,number);strcat(current.text,"\"");}
    strcat(current.text,"}}");
    menu_categories_parse(current.text,strlen(current.text));current.configured=0;
}
size_t menu_categories_count(void) {return current.count;}
const char *menu_category_name(size_t index) {return index<current.count?current.rows[index].name:"";}
int menu_category_hidden(size_t index) {return index<current.count && current.rows[index].hide;}
int menu_categories_configured(void) {return current.configured;}
const char *menu_categories_text(void) {return current.text;}
int menu_category_toggle(size_t index)
{
    static char text[MENU_CATEGORIES_BYTES];
    size_t pos, length, before, after;
    if(index>=current.count) return -1;
    pos=current.rows[index].hidden;before=current.rows[index].hide?4:5;after=current.rows[index].hide?5:4;length=strlen(current.text);
    if(length-before+after>=sizeof(current.text)) return -1;
    memcpy(text,current.text,pos);
    memcpy(text+pos,current.rows[index].hide?"false":"true",after);
    strcpy(text+pos+after,current.text+pos+before);
    return menu_categories_parse(text,length-before+after);
}
int menu_categories_hidden_name(const char *name)
{
    size_t i;for(i=0;i<current.count;++i) if(current.rows[i].hide && !menu_path_compare(name,current.rows[i].name)) return 1;return 0;
}
int menu_categories_hidden_path(const char *path)
{
    size_t i;const char *next;int known=0;
    if(!strncmp(path,"/Storage/",9)) {path+=9;}
    else if(*path=='/') return 0;
    for(i=0;i<current.count;++i) {size_t n=strlen(current.rows[i].name);if(!prefix(path,current.rows[i].name) && (!path[n] || path[n]=='/')) known=1;}
    for(i=0;i<current.count;++i) if(current.rows[i].hide) {
        size_t n=strlen(current.rows[i].name);
        if(!prefix(path,current.rows[i].name) && (!path[n] || path[n]=='/')) return 1;
        /* The ODE may expose a volume component before a category. */
        next=strchr(path,'/');
        if(!known && next && !prefix(next+1,current.rows[i].name) && (!next[n+1] || next[n+1]=='/')) return 1;
    }
    return 0;
}
