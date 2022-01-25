class socket_client
{
protected:
	int fd;

public:
	socket_client();
	virtual ~socket_client();

	virtual int connect() = 0;

	int get_fd();
};
