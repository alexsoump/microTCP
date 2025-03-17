/*
 * microtcp, a lightweight implementation of TCP for teaching,
 * and academic purposes.
 *
 * Copyright (C) 2015-2017  Manolis Surligas <surligas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "microtcp.h"
#include "../utils/crc32.h"

void microtcp_set_mode(microtcp_sock_t *socket, microtcp_mode_t mode) {
  socket->mode = mode;
}

microtcp_sock_t microtcp_socket(int domain, int type, int protocol) {
  
  microtcp_sock_t sock;
  sock.sd = socket(domain, type, protocol);
  if (sock.sd < 0) {
    sock.state = INVALID;
    return sock;
  }

  // Set initial state to UKNOWN
  sock.state = UKNOWN;
  
  // Initialize window sizes
  sock.init_win_size = MICROTCP_WIN_SIZE;
  sock.curr_win_size = MICROTCP_WIN_SIZE;

  // Allocate memory for the receive buffer
  sock.recvbuf = malloc(MICROTCP_RECVBUF_LEN);
  if (!sock.recvbuf) {
    printf("Buffer allocation failed");
    close(sock.sd);
    return sock;
  }

  // Initialize buffer fill level and congestion control parameters
  sock.buf_fill_level = 0;
  sock.cwnd = MICROTCP_INIT_CWND;
  sock.ssthresh = MICROTCP_INIT_SSTHRESH;

  // Initialize sequence and acknowledgment numbers
  sock.seq_number = 0;
  sock.ack_number = 0;

  // Initialize packet and byte counters
  sock.packets_send = 0;
  sock.packets_received = 0;
  sock.packets_lost = 0;
  sock.bytes_send = 0;
  sock.bytes_received = 0;
  sock.bytes_lost = 0;

  sock.mode = MICROTCP_CLIENT;
  
  return sock;
}
  
 

int microtcp_bind(microtcp_sock_t *socket, const struct sockaddr *address,
                  socklen_t address_len) {

  if (bind(socket->sd, address, address_len) < 0) {
    printf("microtcp bind failed");
    return -1;
  }
  socket->state = BOUND;
  return 0;
}

int microtcp_connect(microtcp_sock_t *socket, const struct sockaddr *address,
                     socklen_t address_len) {

  
  if(socket->mode != MICROTCP_CLIENT) {
    fprintf(stderr, "ERROR: microtcp_connect() can only be called in CLIENT mode\n");
    return -1;
  }

  microtcp_header_t syn_packet;
  microtcp_header_t syn_ack_packet;
  microtcp_header_t ack_packet;

  // Prepare SYN packet
  memset(&syn_packet, 0, sizeof(syn_packet));
  syn_packet.control = SYN;
  syn_packet.seq_number = socket->seq_number;

  // Send SYN
  if (sendto(socket->sd, &syn_packet, sizeof(syn_packet), 0, address, address_len) < 0) {
    perror("sendto failed");
    socket->state = INVALID;
    return -1;
  }

  // Wait for SYN-ACK
  if (recvfrom(socket->sd, &syn_ack_packet, sizeof(syn_ack_packet), 0, NULL, NULL) < 0) {
    perror("recvfrom failed");
    socket->state = INVALID;
    return -1;
  }

  // Validate SYN-ACK
  if (syn_ack_packet.control != (SYN | ACK) || syn_ack_packet.ack_number != socket->seq_number + 1) {
    fprintf(stderr, "Invalid SYN-ACK received\n");
    socket->state = INVALID;
    return -1;
  }

  // Prepare ACK
  memset(&ack_packet, 0, sizeof(ack_packet));
  ack_packet.control = ACK;
  ack_packet.seq_number = syn_ack_packet.ack_number;
  ack_packet.ack_number = syn_ack_packet.seq_number + 1;

  // Send ACK
  if (sendto(socket->sd, &ack_packet, sizeof(ack_packet), 0, address, address_len) < 0) {
    perror("sendto failed");
    socket->state = INVALID;
    return -1;
  }

  // Connection established
  socket->state = ESTABLISHED;
  return 0;
 
}

int microtcp_accept(microtcp_sock_t *socket, struct sockaddr *address,
                    socklen_t address_len) {

  if(socket->mode != MICROTCP_SERVER) {
    fprintf(stderr, "ERROR: microtcp_accept() can only be called in SERVER mode\n");
    return -1;
  }


  socklen_t addr_len = address_len;
  microtcp_header_t syn_packet;
  microtcp_header_t syn_ack_packet;

  // Wait for SYN
  if (recvfrom(socket->sd, &syn_packet, sizeof(syn_packet), 0,
               address, &addr_len) < 0) {
    perror("recvfrom failed");
    return -1;
  }

  // Validate SYN
  if (syn_packet.control != SYN) {
    fprintf(stderr, "Invalid SYN received\n");
    return -1;
  }

  // Prepare SYN-ACK
  memset(&syn_ack_packet, 0, sizeof(syn_ack_packet));
  syn_ack_packet.control = SYN | ACK;
  syn_ack_packet.seq_number = socket->seq_number;
  syn_ack_packet.ack_number = syn_packet.seq_number + 1;

  // Send SYN-ACK
  if (sendto(socket->sd, &syn_ack_packet, sizeof(syn_ack_packet), 0, address,
             addr_len) < 0) {
    perror("sendto failed");
    return -1;
  }

  // Wait for ACK
  microtcp_header_t ack_packet;
  if (recvfrom(socket->sd, &ack_packet, sizeof(ack_packet), 0, 
               address, &addr_len) < 0) {
    perror("recvfrom failed");
    return -1;
  }

  // Validate ACK
  if (ack_packet.control != ACK || ack_packet.ack_number != syn_ack_packet.seq_number + 1) {
    fprintf(stderr, "Invalid ACK received\n");
    return -1;
  }

  // Connection established
  socket->state = ESTABLISHED;
  socket->ack_number = ack_packet.ack_number;
  return 0;
 
}

int microtcp_shutdown(microtcp_sock_t *socket, int how) {
  microtcp_header_t fin_packet, ack_packet;
  
  if(socket->state != ESTABLISHED) {
    fprintf(stderr, "Socket is not in ESTABLISHED state\n");
    return -1;
  }

  if(socket->mode == MICROTCP_CLIENT){
    // Client Initiates FIN-ACK Termination
    printf("Client: Initiating connection termination...\n");

    memset(&fin_packet, 0, sizeof(fin_packet));
    fin_packet.control = FIN;
    fin_packet.seq_number = socket->seq_number;
    fin_packet.ack_number = socket->ack_number;

    // Send FIN
    if (sendto(socket->sd, &fin_packet, sizeof(fin_packet), 0, NULL, 0) < 0) {
      perror("sendto failed");
      return -1;
    }

    // Wait for ACK from server
    if (recvfrom(socket->sd, &ack_packet, sizeof(ack_packet), 0, NULL, NULL) < 0) {
      perror("recvfrom failed");
      return -1;
    }

    // Validate ACK
    if (ack_packet.control != ACK || ack_packet.ack_number != fin_packet.seq_number + 1) {
      fprintf(stderr, "Invalid ACK received\n");
      return -1;
    }

    socket->state = CLOSING_BY_HOST;

    // Wait for FIN from server
    if (recvfrom(socket->sd, &fin_packet, sizeof(fin_packet), 0, NULL, NULL) < 0) {
      perror("recvfrom failed");
      return -1;
    }

    // Validate FIN
    if (fin_packet.control != FIN) {
      fprintf(stderr, "Invalid FIN received\n");
      return -1;
    }

    //Prepare ACK
    memset(&ack_packet, 0, sizeof(ack_packet));
    ack_packet.control = ACK;
    ack_packet.seq_number = fin_packet.ack_number;
    ack_packet.ack_number = fin_packet.seq_number + 1;

    //Send ACK
    if (sendto(socket->sd, &ack_packet, sizeof(ack_packet), 0, NULL, 0) < 0) {
      perror("sendto failed");
      return -1;
    } 

    //Set connection to CLOSED 
    socket->state = CLOSED;
    printf("Client: Connection closed successfully.\n");
    return 0;
  }else if(socket->mode == MICROTCP_SERVER){
    //Server Responds to Client's FIN-ACK Termination
    printf("Server: Responding to connection termination...\n");

    //Wait for FIN from client
    if (recvfrom(socket->sd, &fin_packet, sizeof(fin_packet), 0, NULL, NULL) < 0) {
      perror("recvfrom failed");
      return -1;
    }

    //Validate FIN  
    if (fin_packet.control != FIN) {
      fprintf(stderr, "Invalid FIN received\n");
      return -1;
    }

    //Prepare ACK
    memset(&ack_packet, 0, sizeof(ack_packet));
    ack_packet.control = ACK;
    ack_packet.seq_number = fin_packet.ack_number;
    ack_packet.ack_number = fin_packet.seq_number + 1;
  

    //Send ACK to client
    if (sendto(socket->sd, &ack_packet, sizeof(ack_packet), 0, NULL, 0) < 0) {
      perror("sendto failed");
      return -1;
    }

    //Set connection to CLOSING_BY_PEER
    socket->state = CLOSING_BY_PEER;

    //Wait for final ACK from client  
    if (recvfrom(socket->sd, &ack_packet, sizeof(ack_packet), 0, NULL, NULL) < 0) {
      perror("recvfrom failed");
      return -1;
    }

    //Validate ACK
    if (ack_packet.control != ACK || ack_packet.ack_number != fin_packet.seq_number + 1) {
      fprintf(stderr, "Invalid final ACK received\n");
      return -1;
    }

    //Set connection to CLOSED
    socket->state = CLOSED;
    printf("Server: Connection closed successfully.\n");
    return 0;
  }

  fprintf(stderr, "Error: Undefined mode in microtcp_shutdown()\n");
  return -1;
}


// PHASE B:


/**
 * Retransmit a packet that was lost during transmission.
 *
 * This function creates a new packet with the same contents as the lost one,
 * but with a new sequence number and checksum. It then transmits the packet
 * again using sendto(). If the transmission fails, it prints an error message.
 * If the transmission is successful, it prints a message indicating that the
 * packet was retransmitted.
 *
 * @param socket The socket to use for retransmission
 * @param lost_seq_number The sequence number of the packet that was lost
 */
void retransmit_lost_packet(microtcp_sock_t *socket, size_t lost_seq_number) {
    microtcp_header_t packet;
    memset(&packet, 0, sizeof(packet));

    size_t lost_chunk_size = min(MICROTCP_MSS, socket->seq_number - lost_seq_number); //avoid edge case: lost packet is the last one

    // Set packet sequence number to the lost one
    packet.seq_number = lost_seq_number;
    packet.ack_number = socket->ack_number;
    packet.window = socket->curr_win_size;
    packet.data_len = lost_chunk_size;  

    // Compute checksum
    packet.checksum = crc32(0, buffer + (lost_seq_number - socket->seq_number), MICROTCP_MSS);

    // Retransmit the lost packet
    if (sendto(socket->sd, &packet, sizeof(packet), 0, NULL, 0) < 0) {
        perror("sendto failed during retransmission");
    } else {
        fprintf(stderr, "Retransmitted packet with seq %lu\n", lost_seq_number);
    }
}

ssize_t microtcp_send(microtcp_sock_t *socket, const void *buffer,
                      size_t length, int flags) {

    size_t remaining = length;    //arithmos bytes pou thelei na steilei o xrhsths
    size_t data_sent = 0;
    size_t bytes_to_send;
    size_t chunks, i;

    
    microtcp_header_t packet, ack_packet;
    struct timeval timeout;
    fd_set read_fds;


    int dup_ack_count = 0; //dup-ack counter gia fast recovery
    size_t lost_seq_number; // arithmos xamenou paketou

    // Ensure the connection is established
    if (socket->state != ESTABLISHED) {
        fprintf(stderr, "microtcp_send: Socket is not in ESTABLISHED state\n");
        return -1;
    }

    while (remaining > 0) {


      while(socket->curr_win_size == 0) { //an to window size einai 0 tote perimenei na anoixei to window size (dhladh na mporoume na steiloume)
            printf("microtcp_send: Zero window detected, waiting for receiver...\n");

            // **Send a Zero-Window Probe**
            microtcp_header_t probe_packet;
            memset(&probe_packet, 0, sizeof(probe_packet));
            probe_packet.control = 0;  // No special flags, just a probe
            probe_packet.seq_number = socket->seq_number;
            probe_packet.ack_number = socket->ack_number;
            probe_packet.window = socket->curr_win_size;
            probe_packet.data_len = 1;  // stelne dokimastika paketa gia na pareis pisw ena ack opote einai etoimo na dexthei paketa to etsi
            probe_packet.checksum = crc32(0, buffer, 1);

            if (sendto(socket->sd, &probe_packet, sizeof(probe_packet), 0, NULL, 0) < 0) {
                perror("sendto failed (zero-window probe)");
                return -1;
            }

            // **Wait for an ACK with a larger window size**
            FD_ZERO(&read_fds);
            FD_SET(socket->sd, &read_fds);
            timeout.tv_sec = 1;  // stelnetai 1 paketo ana second
            timeout.tv_usec = 0;

            int result = select(socket->sd + 1, &read_fds, NULL, NULL, &timeout);
            if (result > 0) {
                if (recvfrom(socket->sd, &ack_packet, sizeof(ack_packet), 0, NULL, NULL) < 0) {
                    perror("recvfrom failed (zero-window probe response)");
                    return -1;
                }

                // **Check if the window size has increased**
                socket->curr_win_size = ack_packet.window;
                if (socket->curr_win_size > 0) {
                    printf("microtcp_send: Receiver opened window, resuming transmission...\n");
                }
            }
        }

        // Determine how much we can send
        /*curr_win_size = posa bytes borei na dexthei to destination,
        cwnd = congestion window tou socket (den ginetai na steiloume parapanw gt alliws to diktyo tha exei symforisi),
        remaining = posa bytes mporoume na steiloume akoma */
        bytes_to_send = min(socket->curr_win_size, socket->cwnd, remaining); 
        /*posa bytes apo ayta pou mporoume na steiloume mporoume na steiloume se kathe prospelasi*/
        chunks = bytes_to_send / MICROTCP_MSS;

        for(i = 0; i < chunks; i++) {
            memset(&packet, 0, sizeof(packet));
            packet.control = 0;  // No special flags, just data
            packet.seq_number = socket->seq_number;
            packet.ack_number = socket->ack_number;
            packet.window = socket->curr_win_size;
            packet.data_len = MICROTCP_MSS;
            packet.checksum = crc32(0, buffer + (i * MICROTCP_MSS), MICROTCP_MSS); //anathesi tou crc32 checksum (error detection)


            // Send the packet
            if (sendto(socket->sd, &packet, sizeof(packet), 0, NULL, 0) < 0) {
                perror("Send error");
                return -1;
            } 
            
            /*anevazei ton arithmo tou seq_number kata to size tou paketou pu esteile gia na kserei poio einai einai to epomeno packet*/
            socket->seq_number += MICROTCP_MSS;  // Advance sequence number
        }

        // Send last chunk (if any remaining)
        if (bytes_to_send % MICROTCP_MSS) {
            size_t last_chunk_size = bytes_to_send % MICROTCP_MSS;

            memset(&packet, 0, sizeof(packet));
            packet.control = 0;
            packet.seq_number = socket->seq_number;
            packet.ack_number = socket->ack_number;
            packet.window = socket->curr_win_size;
            packet.data_len = last_chunk_size;
            packet.checksum = crc32(0, buffer + (chunks * MICROTCP_MSS), last_chunk_size);

            if (sendto(socket->sd, &packet, sizeof(packet), 0, NULL, 0) < 0) {
                perror("sendto failed");
                return -1;
            }

            socket->seq_number += last_chunk_size;
        }

        // **Wait for ACKs**
        for (i = 0; i < chunks + (bytes_to_send % MICROTCP_MSS ? 1 : 0); i++) {
            FD_ZERO(&read_fds);
            FD_SET(socket->sd, &read_fds);
            timeout.tv_sec = 0;
            timeout.tv_usec = MICROTCP_ACK_TIMEOUT_US;

            int result = select(socket->sd + 1, &read_fds, NULL, NULL, &timeout); //flag gia pote erxetai paketo sto socket

            if (result > 0){  // ACK received
                if (recvfrom(socket->sd, &ack_packet, sizeof(ack_packet), 0, NULL, NULL) < 0) {
                    perror("recvfrom failed");
                    return -1;
                }

                if(ack_packet.ack_number == socket->ack_number){
                  dup_ack_count++;
                }else { 
                  dup_ack_count = 0;
                }

                if(dup_ack_count == 3){
                  fprintf(stderr, "Fast Recovery: 3 Duplicate ACKs received\n");
                  socket->ssthresh = socket->cwnd / 2;
                  socket->cwnd = socket->ssthresh + (3 * MICROTCP_MSS);
                  lost_seq_number = ack_packet.ack_number;

                  retransmit_lost_packet(socket,lost_seq_number); // Retransmit last packet
                  /*deleted: dup_ack_count = 0;
                  */
                }


                if (ack_packet.ack_number > socket->seq_number - bytes_to_send) {
                  /*epivevaiwsh twn bytes pou epivevaiwthikan (prosthiki tus sto data_sent)
                  kai afairesi tous apo ta remaining bytes
                  socket->ack_number = poia bytes perimenei o receiver */
                    data_sent += ack_packet.ack_number - (socket->seq_number - bytes_to_send);
                    remaining -= ack_packet.ack_number - (socket->seq_number - bytes_to_send);
                    socket->ack_number = ack_packet.seq_number;
                }
            } else {  // Timeout - Retransmit last chunk
                fprintf(stderr, "microtcp_send: ACK timeout, retransmitting last segment\n");
                i--;  // Decrement loop counter to retransmit the last segment
            }
        }

        // Update Flow Control Window
        socket->curr_win_size = ack_packet.window;

        // **Congestion Control**
        if (socket->cwnd < socket->ssthresh) {
            // **Slow Start** (exponential increase)
            socket->cwnd += MICROTCP_MSS;
        } else {
            // **Congestion Avoidance** (linear increase)
            socket->cwnd += MICROTCP_MSS * (MICROTCP_MSS / socket->cwnd);
        }
    }

    return data_sent;
}

ssize_t microtcp_recv(microtcp_sock_t *socket, void *buffer, size_t length, int flags) {
    size_t received_bytes = 0;
    microtcp_header_t packet, ack_packet;
    struct timeval timeout;
    fd_set read_fds;

    if (socket->state != ESTABLISHED) {
        fprintf(stderr, "microtcp_recv: Socket is not in ESTABLISHED state\n");
        return -1;
    }

    while (received_bytes < length) {
        FD_ZERO(&read_fds);
        FD_SET(socket->sd, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = MICROTCP_ACK_TIMEOUT_US;

        int result = select(socket->sd + 1, &read_fds, NULL, NULL, &timeout);

        if (result > 0) {  // Data available
            if (recvfrom(socket->sd, &packet, sizeof(packet), 0, NULL, NULL) < 0) {
                perror("recvfrom failed");
                return -1;
            }

            // **Check for FIN Packet (Connection Termination)**
            if (packet.control & FIN) {
                printf("microtcp_recv: Received FIN, closing connection\n");
                socket->state = CLOSING_BY_PEER;
                return -1;
            }

            // **Validate Checksum**
            uint32_t computed_checksum = crc32(0, buffer, packet.data_len);
            if (computed_checksum != packet.checksum) {
                fprintf(stderr, "microtcp_recv: Packet checksum error, ignoring packet\n");
                continue;
            }

            // **Handling In-Order Packet**
            if (packet.seq_number == socket->ack_number) {
                // Copy valid in-order packet to buffer
                memcpy(buffer + received_bytes, buffer, packet.data_len);
                received_bytes += packet.data_len;
                socket->ack_number += packet.data_len;

                // **Check and merge buffered out-of-order packets**
                microtcp_buffered_packet_t *prev = NULL;
                microtcp_buffered_packet_t *curr = socket->out_of_order_head;  //se periptwsi pou yparxei out-of-order paketo

                while (curr) {                                                //epanasynarmologisi paketwn gia na mpoun sto buffer me thn swsth seira
                    if (curr->seq_number == socket->ack_number) {
                        // Merge into buffer
                        memcpy(buffer + received_bytes, curr->data, curr->data_len);
                        received_bytes += curr->data_len;
                        socket->ack_number += curr->data_len;

                        // Remove node from linked list
                        if (prev) {
                            prev->next = curr->next;
                        } else {
                            socket->out_of_order_head = curr->next;
                        }

                        free(curr->data);
                        microtcp_buffered_packet_t *temp = curr;
                        curr = curr->next;
                        free(temp);
                    } else {
                        prev = curr;
                        curr = curr->next;
                    }
                }
            }

            // **Handling Out-of-Order Packet**
            else if (packet.seq_number > socket->ack_number) {          //eftase ena out-of-order paketo
                // Create new buffered packet
                microtcp_buffered_packet_t *new_packet = (microtcp_buffered_packet_t *)malloc(sizeof(microtcp_buffered_packet_t));
                new_packet->seq_number = packet.seq_number;
                new_packet->data_len = packet.data_len;
                new_packet->data = (char *)malloc(packet.data_len);
                memcpy(new_packet->data, buffer, packet.data_len);
                new_packet->next = NULL;

                // Insert into linked list in sorted order
                if (!socket->out_of_order_head || socket->out_of_order_head->seq_number > packet.seq_number) {
                    new_packet->next = socket->out_of_order_head;
                    socket->out_of_order_head = new_packet;  //mpainei to paketo stin lista me ta out-of-order paketa
                } else {
                    microtcp_buffered_packet_t *curr = socket->out_of_order_head;
                    while (curr->next && curr->next->seq_number < packet.seq_number) {
                        curr = curr->next;
                    }
                    new_packet->next = curr->next;
                    curr->next = new_packet;
                }
            }

            // **Handling Duplicate Packet**
            else if (packet.seq_number < socket->ack_number) {
                fprintf(stderr, "microtcp_recv: Duplicate or old packet received\n");
            }

            // **Send ACK**
            memset(&ack_packet, 0, sizeof(ack_packet));
            ack_packet.control = ACK;
            ack_packet.seq_number = socket->seq_number;
            ack_packet.ack_number = socket->ack_number;
            ack_packet.window = socket->curr_win_size;

            if (sendto(socket->sd, &ack_packet, sizeof(ack_packet), 0, NULL, 0) < 0) {
                perror("sendto failed");
                return -1;
            }

        } else {
            fprintf(stderr, "microtcp_recv: No data received, timeout\n");
            return received_bytes;  // Return partial data if timeout occurs
        }
    }

    return received_bytes;
}


