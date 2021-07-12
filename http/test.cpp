#include <stdio.h>
#include <string.h>

void line123(char* text);

using namespace std;
int main(int argc, char const *argv[])
{
  
    char line[] = "GET /somedir/page.html HTTP/1.1";
    line123(line);
    
    return 0;
}

void line123(char *text)
{
    char *m_url = strpbrk(text, " \t");
    *m_url++='\0';
    printf("%s", text);

}
