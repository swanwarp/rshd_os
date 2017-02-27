#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <termios.h>
#include <fstream>
#include <signal.h>
#include <memory.h>
#include <memory>
#include <iostream>
#include <vector>
#include <sys/wait.h>
#include <map>

using namespace std;

#define BUFFER_SIZE 512
#define QUEUE_SIZE 32
#define EPOLL_MAX_EVENTS 16

struct buffer_box {
    char* buff;
    int BUFFER_LEN;
    int offset, len;

    buffer_box(int len): BUFFER_LEN(len), offset(0), len(0) {
        buff = new char[len];
    }

    void reset() {
        offset = 0;
        len = 0;
    }

    void set(const char* data, int length) {
        memcpy(buff, data, length);
        offset = 0;
        len = length;
    }

    ~buffer_box() {
        delete buff;
    }
};

std::map<int, buffer_box*> buffers;

struct descriptor {
    descriptor(int fd) :
            fd(fd)
    {}

    ~descriptor() {
        delete buffers[fd];
        buffers.erase(fd);
        std::cout << "closing fd" << std::endl;
        close(fd);
    }

    int fd;
};

enum class box_type {
    listener, socket, terminal
};

struct descriptor_box {

    descriptor_box(int fd, box_type type) :
            fd(fd),
            type(type),
            write_blocked(false),
            read_blocked(false),
            write_set(false),
            read_set(false)
    {
        buffer_box* buf = new buffer_box(BUFFER_SIZE);
        buffers[fd] = buf;
    }

    descriptor_box* other;
    descriptor fd;

    int sh_pid;

    bool write_blocked;
    bool write_set;
    bool read_blocked;
    bool read_set;

    box_type type;

    int read_fd() {
        char buffer[BUFFER_SIZE];
        int x = read(fd.fd, buffer, BUFFER_SIZE);

        if (x > 0) {
            buffers[other->fd.fd]->set(buffer, x);

            int write_res = other->write_fd();

            if(write_res == 0) {
                std::cout << "Blocking Read, Unlocking Write" << std::endl;

                read_blocked = true;
                read_set = true;

                other->write_blocked = false;
                other->write_set = true;
            }
        } else {
            if(x == 0) {
                return -1;
            } else if(x == -1) {
                if(errno != EAGAIN) {
                    return -1;
                } else {
                    return x;
                }
            }
        }

        return x;
    }

    int write_fd() {
        while (buffers[fd.fd]->len > 0) {
            int x = write(fd.fd, buffers[fd.fd]->buff + buffers[fd.fd]->offset, buffers[fd.fd]->len);

            if (x == -1) {
                if (errno == EWOULDBLOCK) {
                    return 0;
                } else {
                    return -1;
                }
            } else if (x == buffers[fd.fd]->len) {
                buffers[fd.fd]->reset();

                std::cout << "Blocking Write, Unlocking Read" << std::endl;

                write_set = true;
                write_blocked = true;

                other->read_blocked = false;
                other->read_set = true;
            } else {
                buffers[fd.fd]->len -= x;
                buffers[fd.fd]->offset += x;
            }
        }

        return 0;
    }

};

int create_listening_socket(uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if(sock == -1) {
        std::cout << "Failed on creation Socket" << std::endl;
        return -1;
    }

    sockaddr_in s_addr;
    memset(&s_addr, 0, sizeof(sockaddr_in));
    s_addr.sin_family = AF_INET;
    s_addr.sin_port = htons(port);
    s_addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(sock, (const struct sockaddr*) &s_addr, sizeof(sockaddr_in)) < 0) {
        std::cout << "Failed on binding Socket" << std::endl;
        return -1;
    }

    listen(sock, QUEUE_SIZE);
    return sock;
}

int accept_socket(int listening_socket) {
    sockaddr_in accept_data;
    memset(&accept_data, 0, sizeof(sockaddr_in));
    socklen_t len = sizeof(sockaddr_in);

    int client_sock = accept(listening_socket, (sockaddr*) &accept_data, &len);
    return client_sock;
}

void enable_nonblocking(int fd) {
    int status = fcntl(fd, F_GETFD);

    if(fcntl(fd, F_SETFL, status | O_NONBLOCK) == -1) {
        std::cout << "Failed on setting NONBLOCKING" << std::endl;
    }

}

int create_epoll(descriptor_box* listener) {
    int epoll_descriptor = epoll_create(EPOLL_MAX_EVENTS);

    if(epoll_descriptor == -1) {
        std::cout << "Failed on creating Epoll" << std::endl;
        exit(EXIT_FAILURE);
    }

    epoll_event event;
    event.events = EPOLLIN;
    event.data.ptr = (void*) listener;

    if(epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, listener->fd.fd, &event) == -1) {
        std::cout << "Failed on setting Listener Event" << std::endl;
        close(epoll_descriptor);
        exit(EXIT_FAILURE);
    }

    return epoll_descriptor;
}

void add_to_epoll(int epoll_descriptor, descriptor_box* client) {
    epoll_event event;
    event.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLET | EPOLLRDHUP;
    event.data.ptr = (void*) client;

    if(epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, client->fd.fd, &event) == -1) {
        std::cout << "Failed on adding Client/Terminal" << std::endl;
        exit(EXIT_FAILURE);
    }
}


void modify_epoll(int epoll_descriptor, descriptor_box* client) {
    epoll_event event;
    event.events = 0;

    if(!client->write_blocked) {
        event.events = event.events | EPOLLIN | EPOLLET;
    }

    if(!client->read_blocked) {
        event.events = event.events | EPOLLOUT | EPOLLET;
    }

    event.data.ptr = (void*) client;

    if(epoll_ctl(epoll_descriptor, EPOLL_CTL_MOD, client->fd.fd, &event) == -1) {
        std::cout << "Failed on modifying Client/Terminal" << std::endl;
        exit(EXIT_FAILURE);
    }
}

int create_master_terminal() {
    int fdm = posix_openpt(O_RDWR);

    if(fdm < 0) {
        std::cout << "Failed on open Terminal" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (grantpt(fdm) || unlockpt(fdm)) {
        std::cout << "Failed on unlock Terminal" << std::endl;
        exit(EXIT_FAILURE);
    }

    return fdm;
}


std::string const daemon_file = "/tmp/rshd.pid";
std::string const daemon_err_log = "/tmp/rshd.err.log";

void demonize() {
    std::ifstream inf(daemon_file);

    if (inf) {
        int pid;
        inf >> pid;

        if (!kill(pid, 0)) {
            std::cerr << "Daemon is already running with PID " << pid << std::endl;
            exit(pid);
        }
    }

    inf.close();

    auto res = fork();
    if (res == -1) {
        perror("Fork FAIL");
        exit(EXIT_FAILURE);
    }

    if (res != 0) {
        exit(EXIT_SUCCESS);
    }

    setsid();

    int daemon_pid = fork();

    if (daemon_pid) {
        std::ofstream ouf(daemon_file, std::ofstream::trunc);
        ouf<<daemon_pid;
        ouf.close();
        exit(EXIT_SUCCESS);
    }

    int slave = open("/dev/null", O_RDWR);
    int err = open(daemon_err_log.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);

    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(err, STDERR_FILENO);
    close(slave);
    close(err);

    return;
}


std::vector<std::shared_ptr<descriptor_box>> clients;
std::vector<std::shared_ptr<descriptor_box>> terminals;

int main(int argc, char** argv) {
    if(argc < 2) {
        std::cout << "Wow, no port" << std::endl;
        exit(EXIT_FAILURE);
    }

    demonize();

    uint16_t port = atoi(argv[1]);

    auto listener = std::make_shared<descriptor_box>(create_listening_socket(port), box_type::listener);
    int epoll_descriptor = create_epoll(&(*listener));

    descriptor epoll_container(epoll_descriptor);

    while(true) {
        epoll_event events[EPOLL_MAX_EVENTS];
        int events_num = epoll_wait(epoll_descriptor, events, EPOLL_MAX_EVENTS, -1);
        for(int i = 0; i < events_num; i++) {
            descriptor_box* cont = (descriptor_box*) events[i].data.ptr;

            if(cont->type == box_type::listener) {
                std::cout << "New Client connected" << std::endl;

                clients.push_back(std::make_shared<descriptor_box>(accept_socket(listener->fd.fd), box_type::socket));
                terminals.push_back(std::make_shared<descriptor_box>(create_master_terminal(), box_type::terminal));

                clients.back()->other = &(*terminals.back());
                terminals.back()->other = &(*clients.back());

                add_to_epoll(epoll_descriptor, &(*clients.back()));
                add_to_epoll(epoll_descriptor, &(*terminals.back()));

                enable_nonblocking(clients.back()->fd.fd);
                enable_nonblocking(terminals.back()->fd.fd);


                int slave = open(ptsname(terminals.back()->fd.fd), O_RDWR);
                auto proc = fork();

                if (!proc) {
                    clients.clear();
                    terminals.clear();
                    listener.reset();
                    close(epoll_descriptor);

                    struct termios slave_orig_term_settings;
                    struct termios new_term_settings;
                    tcgetattr(slave, &slave_orig_term_settings);
                    new_term_settings = slave_orig_term_settings;
                    new_term_settings.c_lflag &= ~(ECHO | ECHONL | ICANON);

                    tcsetattr (slave, TCSANOW, &new_term_settings);

                    dup2(slave, STDIN_FILENO);
                    dup2(slave, STDOUT_FILENO);
                    dup2(slave, STDERR_FILENO);
                    close(slave);

                    setsid();

                    ioctl(0, TIOCSCTTY, 1);

                    execlp("/bin/sh","sh", NULL);
                } else {
                    close(slave);
                    clients.back()->sh_pid = proc;
                }

            } else {
                int res = -1;

                std::cout << "Dealing with Client" << std::endl;

                if((events[i].events & EPOLLRDHUP) != 0) {
                    std::cout << "Ctrl + D or EOF" << std::endl;
                    res = -1;
                } else if((events[i].events & EPOLLIN) != 0) {
                    std::cout << "Event READ" << std::endl;

                    res = cont->read_fd();

                    if(res == -1) {
                        std::cout << "Res -1 after Read" << std::endl;
                    }
                } else if((events[i].events & EPOLLOUT) != 0) {
                    std::cout << "Event WRITE" << std::endl;

                    res = cont->write_fd();

                    std::cout << "Write Finished" << std::endl;

                    if(res == -1) {
                        std::cout << "Res -1 after Write" << std::endl;
                    }
                } else if((events[i].events & EPOLLERR) != 0) {
                    std::cout << "Event ERR" << std::endl;
                }

                if(cont->write_set || cont->read_set) {
                    cont->write_set = false;
                    cont->read_set = false;

                    modify_epoll(epoll_descriptor, cont);
                }

                if(cont->other->write_set || cont->other->read_set) {
                    cont->other->write_set = false;
                    cont->other->read_set = false;

                    modify_epoll(epoll_descriptor, cont->other);
                }

                if(res == -1) {
                    std::cerr << "Disconnecting Client" << std::endl;

                    descriptor_box* cterm = cont->other;

                    if(cterm->type == box_type::socket) {
                        std::swap(cterm, cont);
                    }

                    for(auto it = clients.begin(); it!=clients.end(); ++it) {
                        if (it->get() == cont) {
                            epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, (*it)->fd.fd, events+i);

                            clients.erase(it);
                            break;
                        }
                    }

                    for(auto it = terminals.begin(); it!=terminals.end(); ++it) {
                        if (it->get() == cterm) {
                            epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, (*it)->fd.fd, events+i);
                            terminals.erase(it);
                            break;
                        }
                    }

                    if(kill((cont) -> sh_pid, SIGINT) == -1) {
                        std::cout << "Cant kill Sh" << std::endl;
                    } else {
                        waitpid((cont) -> sh_pid, 0, 0);
                        std::cout << "Killed Sh with PID " << (cont) -> sh_pid << std::endl;
                    }
                }
            }
        }
    }
}