import socket
import argparse  # added import

def main():
    parser = argparse.ArgumentParser(description="Send a TCP message to the server")
    parser.add_argument("message", help="Message to send to the server")
    args = parser.parse_args()
    message = args.message  # replacing fixed message

    HOST = "192.168.87.109"  # Replace with the server's IP address
    PORT = 8070  # Replace with the server port number
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        s.sendall(message.encode())
        print("Message sent to the TCP server.")

if __name__ == '__main__':
    main()
