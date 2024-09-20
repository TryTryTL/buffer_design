
#include "reactor.h"

void read_cb(int fd, int events, void *privdata) {
    event_t *e = (event_t *)privdata;
    int n = event_buffer_read(e); // 将网络中读缓冲区的数据拷贝到用户态缓冲区
    if (n > 0) {
        // buffer_search 检测是否是一个完整的数据包
        int len = buffer_search(evbuf_in(e), "\n", 1);
        if (len > 0 && len < 1024) {
            char buf[1024] = {0};
            buffer_remove(evbuf_in(e), buf, len);
            event_buffer_write(e, buf, len);
        }
    }
}

void accept_cb(int fd, int events, void *privdata) {
    event_t *e = (event_t*) privdata;

    struct sockaddr_in addr;
	memset(&addr, 0, sizeof(struct sockaddr_in));
	socklen_t len = sizeof(addr);
	
	int clientfd = accept(fd, (struct sockaddr*)&addr, &len);
	if (clientfd <= 0) {
        printf("accept failed\n");
        return;
    }

	char str[INET_ADDRSTRLEN] = {0};
	printf("recv from %s at port %d\n", inet_ntop(AF_INET, &addr.sin_addr, str, sizeof(str)),
		ntohs(addr.sin_port));

    event_t *ne = new_event(event_base(e), clientfd, read_cb, 0, 0);
    add_event(event_base(e), EPOLLIN, ne);
    set_nonblock(clientfd);
}

int main() {
    reactor_t *R = create_reactor();

    if (create_server(R, 8888, accept_cb) != 0) {
        release_reactor(R);
        return 1;
    }

    eventloop(R);
    release_reactor(R);
    return 0;
}
