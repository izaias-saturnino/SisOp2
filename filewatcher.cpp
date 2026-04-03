#include <stdio.h>
#include <iostream>
#include <sys/inotify.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h> 
#include <string>
#include <vector>
#include <mutex>

#define FILE_CREATED 1
#define FILE_DELETED 2
#define FILE_MODIFIED 3

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

using namespace std;

int inotify_fd,watch_dir;

string command ="get_sync_dir";
bool command_complete = true;

vector<string> name;
vector<int> action;
mutex mtx_file_manipulation;

void *folderchecker(void *arg)
{
    inotify_fd = inotify_init();


    if (fcntl(inotify_fd, F_SETFL, O_NONBLOCK) < 0)  // error checking for fcntl
       exit(2);

    watch_dir = inotify_add_watch(inotify_fd, "./sync_dir", IN_MODIFY | IN_CREATE | IN_DELETE|IN_MOVED_TO|IN_MOVED_FROM);
    int run;
    if(watch_dir==-1){
        printf("erro");
        run=0;
    }else{
        run=1;
    }

    while (run)
    {
        int length;
        int i = 0;
        char buffer[EVENT_BUF_LEN];
        mtx_file_manipulation.lock();
        length = read(inotify_fd, buffer, EVENT_BUF_LEN);
        while (i < length)
        {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            if (event->len)
            {
                if (event->mask & IN_CREATE)
                {
                    string s = ((char*) &(event->name));
                    name.push_back(s);
                    action.push_back(FILE_CREATED);
                }
                else if (event->mask & IN_DELETE)
                {
                    string s = ((char*) &(event->name));
                    name.push_back(s);
                    action.push_back(FILE_DELETED);
                }
                else if (event->mask & IN_MODIFY)
                {
                    string s = ((char*) &(event->name));
                    name.push_back(s);
                    action.push_back(FILE_MODIFIED);
                }
                else if (event->mask & IN_MOVED_TO)
                {
                    string s = ((char*) &(event->name));
                    name.push_back(s);
                    action.push_back(FILE_CREATED);
                }
                else if (event->mask & IN_MOVED_FROM)
                {
                    string s = ((char*) &(event->name));
                    name.push_back(s);
                    action.push_back(FILE_DELETED);
                }
            }
            i += EVENT_SIZE + event->len;
        }
        mtx_file_manipulation.unlock();
        usleep(20 * 1000);
    }
    return 0;
}

void *input(void *arg)
{
    while(command!="exit"){
        if(command_complete==false){
            std::getline (std::cin,command);
            command_complete = true;
            cout << "command: " << command << endl << endl;
        }
    }
    return 0;
}
