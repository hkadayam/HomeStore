//! Tokio-specific result channel implementation.

use crate::result_channel::{MessageId, ResultSender};
use tokio::sync::mpsc;

/// Message type sent through the channel: (message_id, boxed_result)
pub type ChannelMessage = (MessageId, Box<dyn std::any::Any + Send + 'static>);

/// Tokio-specific result channel type.
/// Owns both sender and receiver - no split ownership!
pub type ResultChannel = crate::result_channel::ResultChannel<
    mpsc::UnboundedSender<ChannelMessage>,
    mpsc::UnboundedReceiver<ChannelMessage>,
>;

/// Create a new result channel with a tokio mpsc channel.
/// Returns a single ResultChannel that owns both sender and receiver.
pub fn create() -> ResultChannel {
    let (tx, rx) = mpsc::unbounded_channel();
    crate::result_channel::ResultChannel::new(tx, rx)
}

impl ResultChannel {
    /// Run the receiver loop - processes incoming cross-reactor results.
    /// This takes the receiver out and runs until the channel closes.
    /// Can only be called once (receiver is taken).
    pub async fn run_receiver_loop(&self) {
        let mut receiver = self.take_receiver()
            .expect("run_receiver_loop called twice");
        
        while let Some((msg_id, result)) = receiver.recv().await {
            self.put_result(msg_id, result);
        }
    }
}

/// Tokio-specific ResultSender implementation
impl ResultSender<mpsc::UnboundedSender<ChannelMessage>> {
    /// Send a result back to the waiting reactor (tokio version).
    pub async fn send_result<R: Send + 'static>(self, result: R) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        self.sender().send((self.msg_id(), Box::new(result)))
            .map_err(|_| Box::new(std::io::Error::new(std::io::ErrorKind::BrokenPipe, "Failed to send result")) as Box<dyn std::error::Error + Send + Sync>)
    }
}


