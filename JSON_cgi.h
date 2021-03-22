#ifndef FAST_CGI_H
#define FAST_CGI_H

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <iostream>
#include "fcgio.h"
#include <thread>
#include <string>
#include <map>

/**
 * C++ wrapper around the fastCGI which is being used by
 * nginx, apache and lighttpd which acts as an external
 * html generator.
 * It sends out JSON packets and receives JSON packets.
 **/
class JSONCGIHandler {
public:
	/**
	 * Callback handler which needs to be implemented by the main
	 * program.
	 **/
	class GETCallback {
	public:
		/**
		 * Needs to return the payload data sent to the web browser.
		 **/
		virtual std::string getDataString() = 0;
		/**
		 * The content type of the payload. That's by default it's
		 * "application/json" but can be overloaded for CSV or text.
		 **/
		virtual std::string getContentType() { return "application/json"; }
	};


	/**
	 * Callback handler which needs to be implemented by the main
	 * program.
	 **/
	class POSTCallback {
	public:
		/**
		 * Needs to return the payload data sent to the web browser.
		 **/
		virtual void hasData(std::map<std::string,std::string>) = 0;
	};
	

	/**
	 * Simple helper function to create a key/value json pairs
	 * for the callback function.
	 **/
	class JSONGenerator {
	public:
		/**
		 * Adds a JSON entry
		 **/
		void add(std::string key, std::string value) {
			if (!firstEntry) {
				json = json + ", ";
			}
			json = json + "\"" + key + "\":";
			json = json + "\"" + value + "\"";
			firstEntry = 0;
		}

		void add(std::string key, double value) {
			if (!firstEntry) {
				json = json + ", ";
			}
			json = json + "\"" + key + "\":";
			json = json + std::to_string(value);
			firstEntry = 0;
		}

		void add(std::string key, float value) {
			add(key, (double)value);
		}

		void add(std::string key, long value) {
			if (!firstEntry) {
				json = json + ", ";
			}
			json = json + "\"" + key + "\":";
			json = json + std::to_string(value);
			firstEntry = 0;
		}

		void add(std::string key, int value) {
			add(key, (long)value);
		}

		/**
		 * Gets the json
		 **/
		std::string getJSON() { return json + "}"; }
		
	private:
		std::string json = "{";
		int firstEntry = 1;
	};
	
public:
	/**
	 * Constructor which inits it and starts the main thread.
	 * Provide an instance of the callback handler which provides the
	 * payload data in return. The optional socketpath variable
	 * can be set to another path for the socket which talks to the
	 * webserver. postCallback is the callback which returns
	 * received json packets as a map.
	 **/
	JSONCGIHandler(GETCallback* argGetCallback,
		       const char socketpath[] = "/tmp/fastcgisocket",
		       POSTCallback* argPostCallback = nullptr) {
		getCallback = argGetCallback;
		postCallback = argPostCallback;
		// set it to zero
		memset(&request, 0, sizeof(FCGX_Request));
		// init the connection
		FCGX_Init();
		// open the socket
		sock_fd = FCGX_OpenSocket(socketpath, 1024);
		// making sure the nginx process can read/write to it
		chmod(socketpath, S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR|S_IWGRP|S_IWOTH);
		// init requests so that we can accept requests
		FCGX_InitRequest(&request, sock_fd, 0);
		// starting main loop
		mainThread = new std::thread(JSONCGIHandler::exec, this);
	}

	/**
	 * Destructor which shuts down the connection to the webserver 
	 * and it also terminates the thread waiting for requests.
	 **/
	~JSONCGIHandler() {
		running = 0;
		shutdown(sock_fd, SHUT_RDWR);
		mainThread->join();
		delete mainThread;
		FCGX_Free(&request, sock_fd);
	}

 private:
	static void exec(JSONCGIHandler* fastCGIHandler) {
		while ((fastCGIHandler->running) && (FCGX_Accept_r(&(fastCGIHandler->request)) == 0)) {
			char * method = FCGX_GetParam("REQUEST_METHOD", fastCGIHandler->request.envp);
			if (method == nullptr) {
				fprintf(stderr,"Please add 'include fastcgi_params;' to the nginx conf.\n");
				throw "JSONCGI parameters missing.\n";
			}
			if (strcmp(method, "GET") == 0) {
				// create the header
				std::string buffer = "Content-type: "+fastCGIHandler->getCallback->getContentType();
				buffer = buffer + "; charset=utf-8\r\n";
				buffer = buffer + "\r\n";
				// append the data
				buffer = buffer + fastCGIHandler->getCallback->getDataString();
				buffer = buffer + "\r\n";
				// send the data to the web server
				FCGX_PutStr(buffer.c_str(), buffer.length(), fastCGIHandler->request.out);
				FCGX_Finish_r(&(fastCGIHandler->request));
			}
			if (strcmp(method, "POST") == 0) {
				long reqLen = 0;
				char * content_length_str = FCGX_GetParam("CONTENT_LENGTH",
									  fastCGIHandler->request.envp);
				if (content_length_str) reqLen = atol(content_length_str)+1;
				char* tmp = new char[reqLen];
				FCGX_GetStr(tmp,reqLen,fastCGIHandler->request.in);
				tmp[reqLen - 1] = 0;
				std::string s = tmp;
				size_t pos = 0;
				std::map<std::string,std::string> postArgs;
				while (1) {
					std::string token;
					pos = s.find("&");
					if (pos == std::string::npos) {
						token = s;
					} else {
						token = s.substr(0, pos);
					}
					size_t pos2 = token.find("=");
					if (pos2 != std::string::npos) {
						std::string key = token.substr(0,pos2);
						std::string value = token.substr(pos2+1,token.length());
						postArgs[key] = value;
					}
					if (pos == std::string::npos) break;
					s.erase(0, pos + 1);
				}
				if (nullptr != fastCGIHandler->postCallback) {
					fastCGIHandler->postCallback->hasData(postArgs);
				}
				delete[] tmp;
				// create the header
				std::string buffer = "Content-type: text/html";
				buffer = buffer + "; charset=utf-8\r\n";
				buffer = buffer + "\r\n";
				// append the data
				buffer = buffer + "\r\n";
				buffer = buffer + "<html></html>\r\n";
				// send the data to the web server
				FCGX_PutStr(buffer.c_str(), buffer.length(), fastCGIHandler->request.out);
				FCGX_Finish_r(&(fastCGIHandler->request));
			}
		}
	}

 private:
	FCGX_Request request;
	int sock_fd = 0;
	int running = 1;
	std::thread* mainThread = nullptr;
	GETCallback* getCallback = nullptr;
	POSTCallback* postCallback = nullptr;
};

#endif