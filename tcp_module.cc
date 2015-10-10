#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "Minet.h"
#include "tcpstate.h"
#include "tcp.h"
#include "sockint.h"
#include <iostream>

using namespace std;

#define FLAG_SYN 0
#define FLAG_SYN_ACK 1
#define FLAG_ACK 2
#define FLAG_PSH_ACK 3
#define FLAG_FIN 4
#define FLAG_FIN_ACK 5
#define FLAG_RST 6


void MakePacket(Packet &packet, ConnectionToStateMapping<TCPState> & TCPstate, int dataLength, int flagSelector);
void MuxProcess(const MinetHandle &mux, const MinetHandle &sock, ConnectionList<TCPState> &clist);
void SockProcess(const MinetHandle &mux, const MinetHandle &sock, ConnectionList<TCPState> &clist);
void SockReply(const MinetHandle &sock, SockRequestResponse &Sock_Request_Response, Connection conn, unsigned int bytes, srrType type);
void TimeoutProcess(const MinetHandle & mux, ConnectionList<TCPState> & connectionList, ConnectionList<TCPState>::iterator c2s);

void MakePacket(Packet &packet, ConnectionToStateMapping<TCPState> & TCPstate, int dataLength, int flagSelector)
{
	cerr<<"In Packet Making"<<endl;

	unsigned char flagResults = 0;
	int packetLength = dataLength + TCP_HEADER_BASE_LENGTH + IP_HEADER_BASE_LENGTH;
	/* set TCP header Flags */
	switch (flagSelector)
	{
		case FLAG_SYN:
		SET_SYN(flagResults);
		break;

		case FLAG_SYN_ACK:
		SET_ACK(flagResults);
		SET_SYN(flagResults);
		break;

		case FLAG_ACK:
		SET_ACK(flagResults);
		break;

		case FLAG_PSH_ACK:
		SET_PSH(flagResults);
		SET_ACK(flagResults);
		break;

		case FLAG_FIN:
		SET_FIN(flagResults);
		break;

		case FLAG_FIN_ACK:
		SET_FIN(flagResults);
		SET_ACK(flagResults);
		break;

		case FLAG_RST:
		SET_RST(flagResults);
		break;

		default:
		break;
	}
	/* make IP header*/
	IPHeader IPheader;
	IPheader.SetSourceIP(TCPstate.connection.src);
	IPheader.SetDestIP(TCPstate.connection.dest);
	IPheader.SetTotalLength(packetLength);
	IPheader.SetProtocol(IP_PROTO_TCP);
	packet.PushFrontHeader(IPheader);

	/* make TCP header*/
	TCPHeader TCPheader;
	TCPheader.SetSourcePort(TCPstate.connection.srcport, packet);
	TCPheader.SetDestPort(TCPstate.connection.destport, packet);
	TCPheader.SetHeaderLen(TCP_HEADER_BASE_LENGTH/4, packet);
	TCPheader.SetFlags(flagResults, packet);
	TCPheader.SetAckNum(TCPstate.state.GetLastRecvd(), packet);
	TCPheader.SetSeqNum(TCPstate.state.GetLastAcked()+1, packet);
	TCPheader.SetWinSize(TCPstate.state.GetN(), packet);
	TCPheader.SetUrgentPtr(0, packet);
	packet.PushBackHeader(TCPheader);
}

void TimeoutProcess(const MinetHandle & mux, ConnectionList<TCPState> & connectionList, ConnectionList<TCPState>::iterator c2s)
{
  	/*if (c2s->state.ExpireTimerTries() || c2s->state.GetState()==SYN_SENT || c2s->state.GetState()==TIME_WAIT) 
  	{
    	connectionList.erase(c2s);
  	}
  	else*/ 
  	if(c2s->state.GetState() == ESTABLISHED && c2s->bTmrActive == true && c2s->timeout <= Time()) 
  	{
    	if (c2s->state.SendBuffer.GetSize() == 0)
    	{
    		c2s->bTmrActive = false;
    	}
    	else
    	{
	    	
	    	c2s->timeout = Time() + 4;
	    	//ConnectionToStateMapping<TCPState>& m = TCPstate;

	    	int sizeLeft = c2s->state.SendBuffer.GetSize();
	    	Buffer sendBuffer = c2s->state.SendBuffer;
	    	//int currentIndex = 0;
	    	cerr<<"Timeout SendBuffer:  "<<sendBuffer<<endl;
	    	//Retransmit data from LastAcked to LastSent
	    	while(sizeLeft > 0) 
	      	{
	        	//Resend the data, get the payload
	        	unsigned int currentSize = sizeLeft < TCP_MAXIMUM_SEGMENT_SIZE ? sizeLeft:TCP_MAXIMUM_SEGMENT_SIZE;
	        	Packet packet(sendBuffer.ExtractFront(currentSize));
	        	MakePacket(packet, (*c2s), currentSize, FLAG_PSH_ACK);
	        
		        //Send the TCP segment out to mux
		        MinetSend(mux, packet);

		        sizeLeft -= currentSize;
		        //currentIndex += TCP_MAXIMUM_SEGMENT_SIZE;
	      	}
	    }
  	}
}


int main(int argc, char * argv[]) {
	MinetHandle mux, sock;
	MinetInit(MINET_TCP_MODULE);
    
  	mux = MinetIsModuleInConfig(MINET_IP_MUX) ?  MinetConnect(MINET_IP_MUX) : MINET_NOHANDLE;  
  	sock = MinetIsModuleInConfig(MINET_SOCK_MODULE) ? MinetAccept(MINET_SOCK_MODULE) : MINET_NOHANDLE;

  	if ( (mux == MINET_NOHANDLE) && (MinetIsModuleInConfig(MINET_IP_MUX)) ) 
  	{
    	MinetSendToMonitor(MinetMonitoringEvent("Can't connect to mux\n"));
    	return -1;
  	}

  	if ( (sock == MINET_NOHANDLE) && (MinetIsModuleInConfig(MINET_SOCK_MODULE)) ) 
  	{
    	MinetSendToMonitor(MinetMonitoringEvent("Can't accept from sock module\n"));
    	return -1;
  	}

  	MinetSendToMonitor(MinetMonitoringEvent("tcp_module handling TCP traffic\n"));

  	ConnectionList<TCPState> connectionList;
  	MinetEvent event;
  	//every 4 seconds, it will check whether there is a real timeout happened or not!
  	double timeout = 4;

  	while (MinetGetNextEvent(event, timeout) == 0) 
  	{
	    if ((event.eventtype != MinetEvent::Dataflow) || (event.direction != MinetEvent::IN)) 
	    {
	      	// timeout ! probably need to resend some packets
	      	//timeoutHandler(event, clist.begin(), mux);
	      	if (event.eventtype == MinetEvent::Timeout) 
		    {
		    	ConnectionList<TCPState>::iterator c2s = connectionList.FindEarliest();
		    	TimeoutProcess(mux, connectionList, c2s);
		    }
		    else
		    {
	      		// if we received an unexpected type of event, print error
	      		MinetSendToMonitor(MinetMonitoringEvent("Unknown event ignored.\n"));
	      	}	
	    }
	    else
	    {
	      	if (event.handle == mux) 
	      	{
	        	// data from IP layer below
	        	cerr<<"Got a mux event."<<endl;
	        	MuxProcess(mux, sock, connectionList);
	      	}
	      	else if (event.handle == sock) 
	      	{
	        	// data from socket layer above
	        	cerr<<"Got a sock event."<<endl;
	        	SockProcess(mux, sock, connectionList);
	      	}
		    
		}
  	}
  	MinetDeinit();
  	return 0;
}

void MuxProcess(const MinetHandle &mux, const MinetHandle &sock, ConnectionList<TCPState> &clist){
  
	Packet packet;

	MinetReceive(mux, packet);

  	Packet out_packet;
  	TCPHeader tcph;
  	IPHeader iph;
  	Connection conn;
    	
    SockRequestResponse repl, req;
  	bool check_sum;      
  	unsigned int acknum;
  	unsigned int seqnum;
  	unsigned short total_len;
  	unsigned char tcph_len;
  	unsigned char iph_len; 
  	unsigned char flags;
  	unsigned short win_size, urgent_p;
  
  	   
  	// extract header info from packet header 
 	packet.ExtractHeaderFromPayload<TCPHeader>(TCPHeader::EstimateTCPHeaderLength(packet));
  	tcph = packet.FindHeader(Headers::TCPHeader);
  
	// Verify checksum
	check_sum = tcph.IsCorrectChecksum(packet);
	// Extract IP Header
	iph = packet.FindHeader(Headers::IPHeader);

  	// Extract Data from IP Header
  	// note that this is flipped around because "source" is interepreted as "this machine"
  	iph.GetDestIP(conn.src);
  	iph.GetSourceIP(conn.dest);
  	iph.GetProtocol(conn.protocol);
  	iph.GetTotalLength(total_len);
  	iph.GetHeaderLength(iph_len);
  
  	// Extract Data from TCP Header
  	tcph.GetSourcePort(conn.destport);        
  	tcph.GetDestPort(conn.srcport);
  	tcph.GetSeqNum(seqnum);
  	tcph.GetAckNum(acknum);
  	tcph.GetFlags(flags);
  	tcph.GetWinSize(win_size);  
  	tcph.GetUrgentPtr(urgent_p);  
  	tcph.GetHeaderLen(tcph_len);
  
  	total_len = total_len - (tcph_len<<2) - (iph_len<<2);
  
  	Buffer buffer = packet.GetPayload().ExtractFront(total_len);
  
  	ConnectionList<TCPState>::iterator cs = clist.FindMatching(conn);
  
  	//Connection exists
  	if (cs != clist.end())
  	{
	    ConnectionToStateMapping<TCPState>& connstate = *cs;
	    unsigned int currentState = (connstate).state.GetState();
	    
	    switch(currentState){
	                        
	    case CLOSED:
	      	//closed.
	      	cerr<<"We are closed"<<endl;
	      	break;
	    case LISTEN:
	    {
	        cerr<<"We are listening!"<<endl;
	        //initialize sender and receiver window pointers
			(connstate).state.SetLastSent(0); 
			(connstate).state.SetLastRecvd(0);
			(connstate).state.SetLastAcked(0);
	        
			if(IS_SYN(flags))
			{
			  	//turn on Timer
			  	(connstate).bTmrActive = true;
		        (connstate).timeout=Time() + 4;
		        (connstate).connection = conn;//update connection
		  
	          	(connstate).state.SetState(SYN_RCVD);
	          	(connstate).state.last_acked = (connstate).state.last_sent-1;
	          	(connstate).state.SetLastRecvd(seqnum + 1);
	          	MakePacket(out_packet, connstate, 0, FLAG_SYN_ACK);//create syn ack packet
	          	MinetSend(mux, out_packet);
		  		sleep(2);
		  		MinetSend(mux, out_packet);
	          	//talk to the socket
	          	SockReply(sock, repl, conn, 0, STATUS); 
	          	break;
	        }
	        else if(IS_FIN(flags)) 
	        {
	          MakePacket(out_packet, connstate, 0, FLAG_FIN_ACK);
	          MinetSend(mux, out_packet);
	        }
	        break;
	    }
	    case SYN_RCVD:
	    {
	        //handle state syn_rcvd
			cerr<<"We are in syn_rcvd!"<<endl;
	        
	        if(IS_ACK(flags)){
	          	if((connstate).state.GetLastSent() == (acknum - 1))
	          	{
	            	cerr<<"We change status to established!"<<endl;
	            	(connstate).state.SetState(ESTABLISHED);
	            	(connstate).state.SetLastAcked(acknum);
	            	(connstate).state.SetSendRwnd(win_size); 
		    		//Talk with the socket
		    		SockReply(sock, repl,conn, 0, WRITE); 
	          	}
	        }
	      	break;
	    }
	    case SYN_SENT:
	    {
	        cerr<<"We are in syn_sent!"<<endl;
	        //SYN_SENT
	        if(IS_SYN(flags) && IS_ACK(flags))
	        {
	          	(connstate).state.SetLastRecvd(seqnum+1);
	          	(connstate).state.SetSendRwnd(win_size);
	          	(connstate).state.SetState(ESTABLISHED);  
	          	(connstate).state.SetLastAcked(acknum);
	          	(connstate).bTmrActive = false;
		  
	          	MakePacket(out_packet, connstate, 0, FLAG_ACK);//create an ack packet
	          	MinetSend(mux, out_packet);
	          	sleep(2);
		  		MinetSend(mux, out_packet);
		  		(connstate).state.SetLastSent((connstate).state.GetLastSent()+1);
		  		//talk to the socket
	          	SockRequestResponse write (WRITE, connstate.connection, buffer, 0, EOK);

	          	MinetSend(sock, write);
	        } 
	        else if (IS_SYN(flags))
	        {
	          	(connstate).state.SetState(SYN_RCVD);
		  		(connstate).state.SetLastRecvd(seqnum);
		  		MakePacket(out_packet, connstate, 0, FLAG_ACK);//create an ack packet
	          	MinetSend(mux, out_packet);
	          
	          	
	        }
	        else if(IS_FIN(flags) && IS_ACK(flags)) 
	        {                                   
	          	(connstate).state.SetState(CLOSE_WAIT);
		  		(connstate).state.SetLastRecvd(seqnum+1);
	            (connstate).timeout=Time() + 4;                           
	          	// Update seq & ack numbers
	          	MakePacket(out_packet, connstate, 0, FLAG_ACK);
	          	MinetSend(mux, out_packet);
	                      
	          	repl.connection=req.connection;
	          	repl.type=STATUS;
	          	if (cs==clist.end()) 
	          	{
	            	repl.error=ENOMATCH;
	          	} 
	          	else
	          	{
	            	repl.error=EOK;
	            	clist.erase(cs);
	          	}	
	          	MinetSend(sock,repl);
	        }
	        break;
	    }
	    case SYN_SENT1:
	    {
	        cerr<<"We are in syn_sent1!"<<endl;
	        break;  
	    }
	    case ESTABLISHED:
	    {
	        cerr<<"We are in established!"<<endl;
	        //Established
	        if (IS_PSH(flags)) 
	        {
	          	if(seqnum == (connstate).state.GetLastRecvd() && check_sum) {
	            	cerr<<"Receive correct data!"<<endl;
	            	(connstate).state.SetLastRecvd(seqnum+total_len);
	            	(connstate).state.SetLastAcked(acknum);
	            	(connstate).state.SetSendRwnd(win_size);
	            	//sending ACK Packet
	            	MakePacket(out_packet, connstate, 0, FLAG_ACK);//create ack packet
	            	MinetSend(mux, out_packet);
	            	//talk to the socket
	            	connstate.state.SetLastSent(connstate.state.GetLastSent()+1);
	            	SockRequestResponse write (WRITE, connstate.connection, buffer, total_len, EOK);
	            	MinetSend(sock,write);
	                                
	        	} 
	        	else  
		        {
		           	// create old ACK Packet, unexpected seq
		            MakePacket(out_packet, connstate, 0, FLAG_ACK);
		            MinetSend(mux,out_packet);
		        }
	        } 
	        else if (IS_FIN(flags)) 
	        {
	          	// Update state given FIN received                                    
	          	(connstate).state.SetLastRecvd(seqnum+1);
	          	MakePacket(out_packet, connstate, 0, FLAG_ACK);//create ack packet
	          	MinetSend(mux, out_packet);

	          	// Update state
	          	(connstate).state.SetState(CLOSE_WAIT);
	          	SockReply(sock,repl,conn, 0, CLOSE); 
	        } 
	        else if (IS_ACK(flags)) 
	        {
	          	cerr<<"Established ack!"<<endl;
	          	
	         	//ack is correct
		 		if (acknum != (connstate).state.GetLastAcked())
		 		{
		 			//update tcp state
	            	(connstate).state.SetLastRecvd((unsigned int)seqnum+total_len);
	            	(connstate).state.SetLastAcked(acknum);
	            	(connstate).state.SetSendRwnd(win_size);
	            	cerr<<"SendBuffer:  "<<(connstate.state.SendBuffer)<<endl;
	            	if ((acknum-1) == connstate.state.GetLastSent()){
		          		connstate.bTmrActive = false;
		          	}else
		          	{
		          		connstate.timeout = Time() + 4;
		          	}
	          	}
	          	
	         	// when the last sent packet is acked, we stop the timer.
	          	
	          
	        } 

	        // Update receiver window
	        (connstate).state.SetSendRwnd(win_size);                             
	        break;    
	    }
	    case LAST_ACK:
	      	if(IS_ACK(flags)) 
	      	{
		        cerr<<"Received last ack!"<<endl;
		        repl.connection = conn;
		        repl.type = WRITE;
		        repl.bytes = 0;
		        if (cs==clist.end()) 
		        {
	          		repl.error=ENOMATCH;
	        	} 
	        	else 
	        	{
		          	cerr<<"conn ok"<<endl;
		          	repl.error=EOK;
		        }
	        	//MinetSend(sock,repl);
	        	(connstate).state.SetState(CLOSING);
	        	cerr<<"send close to sock"<<endl;
	      	}
	      	break;
	    default:
	     	cerr<<"default"<<endl;
	      	break;
	    }
                
  	}
  	else 
  	{
  		//connection dose not exist
    	cerr<<"conn not exist"<<endl;
    	if(IS_FIN(flags)) 
    	{
      		cerr<<conn<<endl;
      		TCPState a_new_state(1, CLOSED, 3);
      		a_new_state.SetLastRecvd(seqnum);
      		a_new_state.last_acked = (acknum-1);
      		ConnectionToStateMapping<TCPState> a_new_map(conn, Time()+4, a_new_state, false);
      		MakePacket(out_packet, a_new_map, 0, FLAG_FIN_ACK);
      		MinetSend(mux, out_packet);
      		//cerr<<"fin ack sent"<<endl;
    	}
    	else if(IS_SYN(flags)) 
    	{
      		cerr<<"syn"<<endl;
    	}
  	}     
}

void SockProcess(const MinetHandle &mux, const MinetHandle &sock, ConnectionList<TCPState> &clist)
{
  	cerr<<"In Sock Process."<<endl;
  	SockRequestResponse Sock_Request_Response;
  	MinetReceive(sock, Sock_Request_Response);
	ConnectionList<TCPState>::iterator newconnection = clist.FindMatching(Sock_Request_Response.connection);
	  
    if(newconnection != clist.end())
  	{
    	Packet out;
      	cerr<<"Newconnection exists!!!"<<endl;
      	int state = newconnection->state.GetState();
      	cerr<<"State is"<<state<<endl;
      	switch(state)
      	{
	      	case LISTEN:
		        cerr<<"LISTEN"<<endl;
				MakePacket(out, *newconnection, 0, FLAG_SYN);
				MinetSend(mux, out);
				newconnection->state.SetState(SYN_SENT);
				newconnection->timeout = Time() + 4;
				break;
	    	case SYN_RCVD:
	    		cerr << "SYN_RCVD" << endl;
	          	if(Sock_Request_Response.type == CLOSE)
	          	{
	            	MakePacket(out, *newconnection, 0, FLAG_FIN);//create an fin packet
	            	MinetSend(mux, out);
	            	newconnection->state.SetLastSent(newconnection->state.GetLastSent() + 1);
	             	newconnection->state.SetState(FIN_WAIT1);
	          	} 
	          	break;
	      	case SYN_SENT:
	          	cerr<<"SYN_SENT"<<endl;
	          	if (Sock_Request_Response.type == CLOSE)
	          	{
	            	clist.erase(newconnection);
	          	}
	          	break;
	      	case ESTABLISHED:
	          	cerr<<"ESTABLISHED"<<endl;
	          	switch(Sock_Request_Response.type)
	          	{
              		case WRITE:
              		{
	                	cerr<<"Write to server"<<endl;
	                    //unsigned bytes = MIN_MACRO(IP_PACKET_MAX_LENGTH-IP_HEADER_BASE_LENGTH-TCP_HEADER_BASE_LENGTH, Sock_Request_Response.data.GetSize());
	                    //unsigned int dataSize = Sock_Request_Response.data.GetSize();
	                    //set maximum sending size
	                    unsigned int sendWindowSize = (newconnection->state.GetLastSent()-newconnection->state.GetLastAcked());
	                    //adding data to sendbuffer
	                    (newconnection->state).SendBuffer.AddBack(Sock_Request_Response.data);
	                    Buffer sendBuffer = (newconnection->state).SendBuffer;

                    	cerr<<"sendbuffer after add:  "<<(newconnection->state).SendBuffer<<endl;
	                    
	                    /*
	                    flow control flow control 
	                    */
	                    while(sendBuffer.GetSize() !=0 && sendWindowSize > (newconnection->state).GetRwnd()){
	                    	Packet packet1(sendBuffer.ExtractFront(1));  // make and send packet with only 1 byte length
	                    	MakePacket(packet1, *newconnection, 1, FLAG_PSH_ACK);
	                    	MinetSend(mux, packet1);
	                    	newconnection->state.SetLastSent(newconnection->state.GetLastSent()+1);
	                    	SockReply(sock, Sock_Request_Response, Sock_Request_Response.connection, 1, STATUS); 
	                    }
	                    
						unsigned int dataSize = sendBuffer.GetSize();
                    	Packet packet(sendBuffer.ExtractFront(dataSize));
                    	//Make PSH ACK Packet
                    	MakePacket(packet, *newconnection, dataSize, FLAG_PSH_ACK); 
	                    // Send Data Packet
	                    MinetSend(mux, packet);
	                    // Set timer for newconnection
	                    newconnection->bTmrActive = true;
	                    newconnection->timeout = Time() + 4;
	                    newconnection->state.SetLastSent(newconnection->state.GetLastSent()+dataSize);
	                    SockReply(sock, Sock_Request_Response, Sock_Request_Response.connection,dataSize, STATUS);                
                  		break;
                  	}
              		case CLOSE:
              		{
                    	MakePacket(out, *newconnection, 0, FLAG_FIN);//create fin packet
                    	MinetSend(mux, out);
                    	newconnection->state.SetLastSent(newconnection->state.GetLastSent() + 1);
                    	newconnection->state.SetState(FIN_WAIT1);
                		break;
                	}
                	case STATUS:
                		break;
              		default:
                		break;
                }
          		break;
      		case CLOSE_WAIT:
	          	cerr<<"CLOSE_WAIT"<<endl;
	          	if(Sock_Request_Response.type == CLOSE)
	          	{
	            	MakePacket(out, *newconnection, 0, FLAG_FIN);//create fin ack packet
	            	newconnection->state.SetState(LAST_ACK);
	            	newconnection->state.SetLastSent(newconnection->state.GetLastSent()+1);
	          	} 
	          	else 
	          	{
	            	cerr << "Shouldn't get here. CLOSE_WAIT" << endl;
	          	}
	          	break;
     		case FIN_WAIT1:
          		cerr<<"FIN_WAIT1"<<endl;
          		if(Sock_Request_Response.type == ACCEPT)
          		{
            		cerr << "Trying to open an existing conneciton." <<endl;
          		}
          		break;
      		case CLOSING:
          		cerr<<"CLOSING "<<endl;
	          	/*
	          	newconnection->state.SetState(TIME_WAIT);
	          	newconnection->state.SetLastSent(newconnection->state.GetLastSent()+1);
	          	*/
		        if(Sock_Request_Response.type == STATUS) {
	            	cerr<<"Status "<<endl;
	          	}
          		else if(Sock_Request_Response.type == CLOSE) 
          		{
	            	cerr<<"Close "<<endl;
	            	clist.erase(newconnection);
		            TCPState a_new_state_close(1, LISTEN, 3);
		            ConnectionToStateMapping<TCPState> a_new_map_close(Sock_Request_Response.connection, Time()+4, a_new_state_close, false);
		            a_new_map_close.bTmrActive = false;
		            //clist.push_back(a_new_map); 
		            
	            	SockReply(sock, Sock_Request_Response,Sock_Request_Response.connection, 0, STATUS);
            	}
          		break;
      		case LAST_ACK:
          		cerr<<"LAST_ACK"<<endl;
          		break;
      		case FIN_WAIT2:
      		{
          		cerr<<"FIN_WAIT2"<<endl;
            	Packet ack;
            	MakePacket(ack, *newconnection, 0, FLAG_ACK);
            	MinetSend(mux,ack);
            	newconnection->state.SetLastSent(newconnection->state.GetLastSent()+1);
            	newconnection->state.SetState(TIME_WAIT);
            }
          		break;
          	
      		case TIME_WAIT:
          		cerr<<"TIME_WAIT"<<endl;
          		break;
      		default:
          		break;
      	}
  	}   
  	else 
  	{
      	cerr<<"Newconnection not exists: "<<Sock_Request_Response.type<<endl;
      	Packet out;
      	switch(Sock_Request_Response.type)
      	{
        	case CONNECT:
        	{
	            cerr<<"CONNECT"<<endl;     
	            TCPState a_new_state_connect(1, SYN_SENT, 3);//after send out sy packet, next state is SYN_SENT
	            ConnectionToStateMapping<TCPState> a_new_map_connect(Sock_Request_Response.connection, Time()+4, a_new_state_connect, false);

	            MakePacket(out, a_new_map_connect, 0, FLAG_SYN);//create syn packet
	            MinetSend(mux, out);
	            sleep(2);
	            MinetSend(mux, out);
				a_new_map_connect.timeout=Time() + 4;
	            a_new_map_connect.state.SetLastSent(a_new_map_connect.state.GetLastSent()+1);
	            clist.push_back(a_new_map_connect);
	            
	            SockReply(sock, Sock_Request_Response,Sock_Request_Response.connection, 0, STATUS);
	            break;
	        }
        	case ACCEPT:
        	{
	            cerr<<"ACCEPT"<<endl;
	            TCPState a_new_state_accept(1, LISTEN, 3);
	            ConnectionToStateMapping<TCPState> a_new_map_accept(Sock_Request_Response.connection, Time()+4, a_new_state_accept, false);
	            a_new_map_accept.bTmrActive = true;
	            clist.push_back(a_new_map_accept);
	            break;
	        }
        	case STATUS:
	            cerr<<"STATUS"<<endl;
	            break;
        	default:
            	break;
      	}
  	}
}

void SockReply(const MinetHandle &sock, SockRequestResponse &Sock_Request_Response, Connection conn,unsigned int bytes, srrType type)
{
    SockRequestResponse reply;
    reply.type = type;
    reply.connection = conn;
    reply.bytes = bytes;
    reply.error = EOK;
    MinetSend(sock, reply);
}








