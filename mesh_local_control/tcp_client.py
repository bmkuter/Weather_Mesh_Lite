import socket

def main():
    HOST = "192.168.87.109"  # Replace with the server's IP address
    PORT =  8070  # Replace with the server port number
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        message = "Hello TCP Server!"
        s.sendall(message.encode())
        print("Message sent to the TCP server.")

if __name__ == '__main__':
    main()
