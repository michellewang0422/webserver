# webserver
We built a functional web server that listens for connections on a socket, allowing clients to connect and use a simple text-based protocol to retrieve files. To handle concurrent requests, we implemented a multithreaded approach. Threads, which represent lightweight "threads of execution," allow for concurrency with minimal context-switching overhead because threads share the same execution environment and address space. This approach offers a performance advantage over processes, which involve heavyweight concurrency due to each process having its own isolated execution environment.

However, using threads also introduces potential downsides. Since threads share state (i.e., address space) within a single process, there is a risk of synchronization issues and deadlocks. Despite this, we opted for threads over processes, which are more resource-intensive, and over event-driven architecture, which—while free of significant synchronization and context-switching overhead—can be harder to design and debug effectively.

Our multithreaded design spawns a new thread for each incoming connection. Inside each thread, we parse the HTTP request, verify its validity, check file permissions, transmit the requested file, and close the connection for HTTP/1.0 requests (or keep it open if necessary). Additionally, we implemented a simple heuristic that dynamically adjusts the timeout period based on the number of active clients, calculated as 10*(1/number_of_clients), ensuring that thread resources are efficiently managed and not consumed for extended periods when the server is under heavy load.
