includelib kernel32.lib
includelib ws2_32.lib

includelib ucrt.lib
includelib legacy_stdio_definitions.lib
includelib vcruntime.lib

.data
	$WSASStartupError db 'WSAStartup failed with error: %d', 0aH, 00H
	$DefaultPort db '27015', 00H
	$WaitingOnPort db 'Waiting On Port: %s', 0aH, 00H
	$GetAddrInfoError db 'getaddrinfo failed with error: %d', 0aH, 00H
	$SocketError db 'socket failed with error: %ld', 0aH, 00H
	$BindError db 'bind failed with error: %d', 0aH, 00H
	$ListenError db 'listen failed with error: %d', 0aH, 00H
	$AcceptError db 'accept failed with error: %d', 0aH, 00H
	$BytesReceived db 'Bytes received: %d', 0aH, 00H
	$BytesSent db 'Bytes sent: %d', 0aH, 00H
	$SendError db 'send failed with error: %d', 0aH, 00H
	$ConnectionClosing db 'Connection closing...', 0aH, 00H
	$ReceiveError db 'recv failed with error: %d', 0aH, 00H
	$ShutDownError db 'shutdown failed with error: %d', 0aH, 00H

.code
externdef MessageBoxA: proc

externdef printf: proc
externdef memset: proc

; Socket functions
externdef WSAStartup: proc
externdef getaddrinfo: proc
externdef WSACleanup: proc
externdef socket: proc
externdef WSAGetLastError: proc
externdef freeaddrinfo: proc
externdef closesocket: proc
externdef bind: proc
externdef listen: proc
externdef accept: proc
externdef recv: proc
externdef send: proc
externdef shutdown: proc

WSADATA STRUCT
    wVersion WORD ?
    wHighVersion WORD ?
    szDescription BYTE 257 DUP(?)
    szSystemStatus BYTE 129 DUP(?)
    iMaxSockets WORD ?
    iMaxUdpDg WORD ?
    lpVendorInfo DQ ?
WSADATA ENDS

result$ = 32
funcResult$ = 40
listenSocket$ = 48
clientSocket$ = 56
sendResult$ = 64
recvbuflen$ = 68
hints$ = 72
wsaData$ = 128
recvbuf$ = 544

main proc
$SETUP1:
	sub rsp, 1080
	
	mov qword ptr ListenSocket$[rsp], -1
	mov qword ptr ClientSocket$[rsp], -1
	mov qword ptr result$[rsp], 0
	mov dword ptr recvbuflen$[rsp], 512
	
	lea rdx, qword ptr wsaData[rsp]
	mov cx, 514
	call WSAStartup
	mov dword ptr funcResult$[rsp], eax
	cmp dword ptr funcResult$[rsp], 0
	je short $SETUP2

	mov edx, dword ptr funcResult$[rsp]
	lea rcx, offset $WSASStartupError
	call printf
	mov rax, 1
	jmp $END1

$SETUP2:
	mov r8d, 48
	mov edx, 0
	lea rcx, qword ptr hints$[rsp]
	call memset
	
	mov dword ptr hints$[rsp+4], 2
	mov dword ptr hints$[rsp+8], 1
	mov dword ptr hints$[rsp+12], 6
	mov dword ptr hints$[rsp], 1
	
	lea r9, qword ptr result$[rsp]
	lea r8, qword ptr hints$[rsp]
	lea rdx, offset $DefaultPort
	mov ecx, 0
	call getaddrinfo
	
	mov dword ptr funcResult$[rsp], eax
	cmp dword ptr funcResult$[rsp], 0
	je short $SETUP3
	
	mov edx, dword ptr funcResult$[rsp]
	lea rcx, offset $GetAddrInfoError
	call printf
	
	call WSACleanup
	mov rax, 1
	jmp $END1
	
$SETUP3:
	mov rax, qword ptr result$[rsp]
	mov r8d, dword ptr [rax+12]
	mov edx, dword ptr [rax+8]
	mov ecx, dword ptr [rax+4]
	call socket	
	mov qword ptr listenSocket$[rsp], rax
	cmp qword ptr listenSocket$[rsp], -1
	jne short $SETUP4

	call WSAGetLastError
	mov edx, eax
	lea rcx, offset $SocketError
	call printf
	
	mov rcx, qword ptr result$[rsp]
	call freeaddrinfo
	call WSACleanup
	mov rax, 1
	jmp $END1

$SETUP4:
	mov rax, qword ptr result$[rsp]
	mov r8d, dword ptr [rax+16]
	mov rax, qword ptr result$[rsp]
	mov rdx, qword ptr [rax+32]
	mov rcx, qword ptr listenSocket$[rsp]
	call bind
	mov dword ptr funcResult$[rsp], eax
	cmp dword ptr funcResult$[rsp], -1
	jne short $SETUP5
	
	call WSAGetLastError
	mov edx, eax
	lea rcx, offset $BindError
	call printf
	
	mov rcx, qword ptr result$[rsp]
	call freeaddrinfo
	mov rcx, qword ptr listenSocket$[rsp]
	call closesocket
	call WSACleanup
	mov rax, 1
	jmp $END1


$SETUP5:
	mov rcx, qword ptr result$[rsp]
	call freeaddrinfo

	mov edx, 2147483647
	mov rcx, qword ptr listenSocket$[rsp]
	call listen
	mov dword ptr funcResult$[rsp], eax
	cmp dword ptr funcResult$[rsp], -1
	jne short $SETUP6 
	
	call WSAGetLastError
	mov edx, eax
	lea rcx, offset $ListenError
	call printf
	
	mov rcx, qword ptr listenSocket$[rsp]
	call closesocket
	call WSACleanup
	mov rax, 1
	jmp $END1
	
$SETUP6:
	lea rdx, offset $DefaultPort
	lea rcx, offset $WaitingOnPort
	call printf

	mov r8d, 0
	mov rdx, 0
	mov rcx, qword ptr listenSocket$[rsp]
	call accept
	mov qword ptr clientSocket$[rsp], rax
	cmp qword ptr clientSocket$[rsp], -1
	jne short $SETUP7
	
	call WSAGetLastError
	mov edx, eax
	lea rcx, offset $AcceptError
	call printf
	
	mov rcx, qword ptr listenSocket$[rsp]
	call closesocket
	call WSACleanup
	mov rax, 1
	jmp $END1

$SETUP7:
	mov rcx, qword ptr listenSocket$[rsp]
	call closesocket
	; npad 1
	
$LOOP1:
	mov r9d, 0
	mov r8d, dword ptr recvbuflen$[rsp]
	lea rdx, qword ptr recvbuf$[rsp]
	mov rcx, qword ptr clientSocket$[rsp]
	call recv
	mov dword ptr funcResult$[rsp], eax
	cmp dword ptr funcResult$[rsp], 0
	jle short $LOOP2
	
	mov edx, dword ptr funcResult$[rsp]
	lea rcx, offset $BytesReceived
	call printf
	
	mov r9d, 0
	mov r8d, dword ptr funcResult$[rsp]
	lea rdx, qword ptr recvbuf$[rsp]
	mov rcx, qword ptr clientSocket$[rsp]
	call send
	mov dword ptr sendResult$[rsp], eax
	cmp dword ptr sendResult$[rsp], -1
	jne short $LOOP3
	
	call WSAGetLastError
	mov edx, eax
	lea rcx, offset $SendError
	call printf
	
	mov rcx, qword ptr clientSocket$[rsp]
	call closesocket
	call WSACleanup
	mov rax, 1
	jmp $END1
	
$LOOP3:
	mov edx, dword ptr sendResult$[rsp]
	lea rcx, offset $BytesSent
	call printf
	jmp short $LOOPEND1
	
	
$LOOP2:
	cmp dword ptr funcResult$[rsp], 0
	jmp short $LOOP4
	lea rcx, offset $ConnectionClosing
	call printf
	jmp short $LOOPEND2
	
$LOOP4:
	call WSAGetLastError
	mov edx, eax
	lea rcx, offset $ReceiveError
	call printf
	mov rcx, qword ptr clientSocket$[rsp]
	call closesocket
	call WSACleanup
	mov rax, 1
	jmp $END1


$LOOPEND2:	
$LOOPEND1:
	cmp dword ptr funcResult$[rsp], 0
	jg $LOOP1
	
	mov edx, 1
	mov rcx, qword ptr clientSocket$[rsp]
	call shutdown
	mov dword ptr sendResult$[rsp], eax
	cmp dword ptr sendResult$[rsp], -1
	jne short $CLEANUP1
	
	call WSAGetLastError
	mov edx, eax
	lea rcx, offset $ShutDownError
	call printf
	
	mov rcx, qword ptr clientSocket$[rsp]
	call closesocket
	call WSACleanup
	mov rax, 1
	jmp $END1
	
$CLEANUP1:
	mov rcx, qword ptr clientSocket$[rsp]
	call closesocket
	call WSACleanup
	
	mov rax, 0
	
$END1:
	add rsp, 1080
	ret 0

main endp
end