use std::{io, pin::Pin, task::{Context, Poll}, time::Duration};
use tokio::{io::{AsyncRead, AsyncWrite, ReadBuf}, time::Sleep};

// A flush completes the pending HTTP output. Partial writes do not renew its
// deadline, so a trickling reader cannot retain an SSE reader slot indefinitely.
pub struct WriteDeadline<T> { stream: T, deadline: Pin<Box<Sleep>>, writing: bool }
impl<T> WriteDeadline<T> {
    pub fn new(stream: T) -> Self {
        Self { stream, deadline: Box::pin(tokio::time::sleep(Duration::from_secs(1))), writing: false }
    }
    fn check(&mut self, cx: &mut Context<'_>) -> io::Result<()> {
        if !self.writing {
            self.deadline.as_mut().reset(tokio::time::Instant::now() + Duration::from_secs(1));
            self.writing = true;
        }
        if self.deadline.as_mut().poll(cx).is_ready() {
            Err(io::Error::new(io::ErrorKind::TimedOut, "HTTP write timeout"))
        } else { Ok(()) }
    }
}
impl<T: AsyncRead + Unpin> AsyncRead for WriteDeadline<T> {
    fn poll_read(mut self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.stream).poll_read(cx, buf)
    }
}
impl<T: AsyncWrite + Unpin> AsyncWrite for WriteDeadline<T> {
    fn poll_write(mut self: Pin<&mut Self>, cx: &mut Context<'_>, bytes: &[u8]) -> Poll<io::Result<usize>> {
        if let Err(error) = self.check(cx) { return Poll::Ready(Err(error)); }
        Pin::new(&mut self.stream).poll_write(cx, bytes)
    }
    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        if self.writing {
            if let Err(error) = self.check(cx) { return Poll::Ready(Err(error)); }
        }
        match Pin::new(&mut self.stream).poll_flush(cx) {
            Poll::Ready(Ok(())) => { self.writing = false; Poll::Ready(Ok(())) }
            result => result,
        }
    }
    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.stream).poll_shutdown(cx)
    }
}
